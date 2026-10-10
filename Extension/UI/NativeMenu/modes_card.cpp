#include "modes_card.h"
#include "native_menu_data.h"
#include "native_menu_internal.h"
#include "native_menu_lifetime.h"
#include "Extension/Modes/game_modes.h"
#include "Extension/Modes/mode_rules.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_menu.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <utility>

// The Throwdowns page holds a LinearList of three card tiles (S.K.A.T.E., Spot Battle, Skate Jam).
// Each game mode gets a card after them, a private copy of the Spot Battle tile. All the cards are
// shown smaller in the row, which scrolls left and right; with `mode grid on` they are laid out in
// a grid instead (build_grid: rows of cards_per_row, scrolling up and down, where the row was). A card's button calls back
// here (the native navigation id is cleared, so it opens no stock setup): the mode is set up and the
// page body swaps to a private copy of the authored Throwdown creation panel whose list holds our
// rows. Leaving restores the page's own body, Back button, title and page actions.
// Model layouts and record names follow the game's authored UI resources (native menu tooling).
namespace dingosdk::multiplayer {
namespace {
using namespace menu_data;
namespace build = game::build::v20260929;
constexpr Schema tile_schema{0xdeb20e0f, 1200}, category_schema{0x628aa99c, 320}, anchor_schema{0x0e3be640, 192},
    label_schema{0xbfe23948, 104}, action_schema{0x85b7eac0, 72}, panel_schema{0xd29bbf63, 576},
    header_schema{0x4efe5e19, 312}, panel_style_schema{0x1b6c904d, 272}, notice_schema{0x7290e514, 256},
    texture_schema{0x370c9975, 144}, page_action_schema{0x896f8a1a, 784};
constexpr std::uint32_t content = 0x716496c8, items = 0x61742cb4, widget = 0x214d4984, rows_field = 0x67223da7,
    text_field = 0x4d8e01b9, handles_field = 0xf2c90867, focus_field = 0x55511280, label_field = 0x58c9d354,
    callback_field = 0x9c76b86c, navigation_field = 0xb1e14cd6, page_actions = 0xd2118ca8, title_path = 0x3781b603;
constexpr std::uint32_t clip_field = 0x4554761c; // LinearList: clip to its bounds (and scroll to focus)

// The stock card whose art a mode's card borrows: 0 S.K.A.T.E., 1 Spot Battle, 2 Skate Jam's plain
// Throwdown mark (also the fallback for a mode not listed here).
unsigned card_art(modes::Mode mode) {
    using modes::Mode;
    switch (mode) {
    case Mode::skate: return 0;
    case Mode::meat:
    case Mode::jam:
    case Mode::one_up:
    case Mode::domination:
    case Mode::graffiti: return 1;
    default: return 2;
    }
}
std::string upper(std::string_view text) {
    std::string out(text);
    for (auto &ch : out) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    return out;
}
// Our card for a mode (from the game modes' own registry, mode_rules.h): a private tile, its
// category and description, and its description label.
struct ModeCard {
    modes::Mode mode{};
    std::string key, name;
    Value tile, category, description, label;
};
using Action = MenuAction;
struct Descriptor {
    alignas(8) std::array<std::byte, 0x70> bytes{};
    Address info{};
    Action action;
};
struct Row {
    Value model, anchor;
    std::string text;
    Address callback{};
};
// A stock card and its authored sizes, put back when our cards go.
struct Original {
    Value tile, category, description;
    std::array<float, 2> tile_size{}, description_size{};
    float icon_width{}, icon_height{};
    Value label;      // its description's label
    std::string text; // and that label's own text, put back when the grid goes
};
// Each card at a share of its authored width and height (and icon size). In one row (the default)
// the cards keep most of their height and the row scrolls left and right; in a grid (`mode grid
// on`, rows of `cards_per_row`) they are shorter, so about two rows show at once and the grid
// scrolls up and down. At .62 wide the longest titles ("DOMINATION", "HALL OF MEAT") fit.
struct CardScale {
    float width, height, icon;
};
// The grid's cards are short (a name and a logo, no description: seen in game 2026-10-09) so all
// four rows fit under the page's title without scrolling; `grid_top` keeps clear of that title.
constexpr CardScale row_scale{.7f, .9f, .7f}, grid_scale{.64f, .27f, .27f};
constexpr unsigned cards_per_row = 4;
constexpr float card_gap = 20.f, grid_top = 150.f;
struct State {
    Address manager{}, base{}, anchor_asset{}, primary_input{};
    Value page, cards, source_label;
    std::vector<ModeCard> mode_cards;
    bool original_clip{};
    Value original_body, original_back, original_actions;
    Value panel_anchor, panel, header, badge, panel_style, panel_notice, footer_anchor, footer_list, footer_back,
        footer_confirm, confirm_action, details_list;
    std::string original_title;
    std::map<std::string, Address, std::less<>> assets;
    OwnedMenuModels<Value> owned;
    std::vector<Original> originals;
    std::map<std::string, Row> rows;
    std::set<std::pair<Handle, Handle>> generations;
    std::vector<std::string> displayed;
    std::array<Descriptor, 64> descriptors;
    unsigned used{};
    std::mutex mutex;
    std::vector<Action> pending;
    std::atomic<std::uint64_t> owner{};
    std::uint64_t next_id{}, next_tick{}, pass{};
    std::string status; // the last game-mode command's answer
    bool details{};
    // The grid: `holder` is the page's presenter that showed the stock card row (`original_holder`
    // its own widget, put back on release); `grid` the vertical list of row presenters shown there
    // instead. Without a grid (it could not be built) our cards join the stock row.
    Value holder, grid;
    Widget original_holder{};
};
State &state() {
    static auto *value = new State;
    return *value;
}
// Forgets every model. The descriptors stay: native buttons may still hold them. A page generation
// already handled is not handled again unless `generations_too` (a new UI manager or level).
void reset(bool generations_too) {
    auto &s = state();
    s.owner.store(0);
    s.anchor_asset = s.primary_input = 0;
    s.page = s.cards = s.source_label = s.holder = s.grid = {};
    s.original_holder = {};
    s.mode_cards.clear();
    s.original_body = s.original_back = s.original_actions = {};
    s.panel_anchor = s.panel = s.header = s.badge = s.panel_style = s.panel_notice = s.footer_anchor = s.footer_list = {};
    s.footer_back = s.footer_confirm = s.confirm_action = s.details_list = {};
    s.original_title.clear();
    s.assets.clear();
    s.originals.clear();
    s.rows.clear();
    s.displayed.clear();
    s.details = false;
    if (generations_too) s.generations.clear();
    std::lock_guard lock(s.mutex);
    s.pending.clear();
}
Value reference(const Context &c, Ref ref) { return {ref.handle, ref.handle ? c.type_of(ref.handle) : 0}; }
Value primary(const Context &c, Value tile) {
    return c.path(tile, {widget, 0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
}
Value back(const Context &c, Value page_value) {
    return c.path(page_value, {0x10810f0b, 0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
}
Value body(const Context &c, Value p) { return c.field(c.element(c.path(p, {content, items}), 0), content); }
Value make(const Context &c, Schema schema, Address authored_type = 0) {
    auto &s = state();
    const auto id = (s.owner.load() ^ 0x524d4f4400000000ULL) + ++s.next_id;
    Value value;
    if (authored_type) {
        require((kind(authored_type) == 2 || kind(authored_type) == 4) && size(authored_type) == schema.size &&
                    read<std::uint32_t>(read<Address>(authored_type)) == schema.hash,
                "Game modes card: authored model schema differs.");
        value = {game::native_data().models.create(c.manager, authored_type, id,
                                                   game::native_name_hash("ReSkate.NativeMultiplayerMenu"), false, 2),
                 authored_type};
        c.address(value);
    } else {
        value = c.create(schema, id);
    }
    s.owned.track(value);
    return value;
}

// Native button callbacks: one fixed function per descriptor slot, each queueing its action.
template <std::size_t I> void activate() noexcept {
    auto &s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (I < s.used && s.descriptors[I].action.generation && s.descriptors[I].action.generation == s.owner.load() &&
            s.pending.size() < 8 && std::none_of(s.pending.begin(), s.pending.end(), [&](const Action &a) {
                return a.command == s.descriptors[I].action.command && a.argument == s.descriptors[I].action.argument;
            }))
            s.pending.push_back(s.descriptors[I].action);
    } catch (...) {}
}
template <std::size_t... I> auto functions(std::index_sequence<I...>) { return std::array{&activate<I>...}; }
Address callback(const Context &c, std::string command, std::string argument = {}) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    const auto slot = action_slot(s.used, s.descriptors.size(), [&](std::size_t i) -> const Action & { return s.descriptors[i].action; },
                                  command, argument, s.pass);
    require(slot.has_value(), "Game modes card: action capacity reached.");
    static const auto f = functions(std::make_index_sequence<64>{});
    auto &d = s.descriptors[*slot];
    if (*slot == s.used) {
        const auto prototype = c.base + build::native_menu::action_prototype;
        require(read<Address>(prototype + 0x40) == c.base + build::native_menu::action_invoker && read<std::uint8_t>(prototype + 0x68) == 0,
                "Game modes card: action ABI differs.");
        require(memory::peek_bytes(prototype, d.bytes.data(), d.bytes.size()), "Game modes card: action prototype unavailable.");
        const auto invoker = reinterpret_cast<Address>(f[*slot]);
        for (const auto offset : {0x30, 0x40, 0x48}) std::memcpy(d.bytes.data() + offset, &invoker, 8);
        d.info = reinterpret_cast<Address>(d.bytes.data());
        ++s.used;
    }
    d.action = {s.owner.load(), std::move(command), std::move(argument), s.pass};
    return reinterpret_cast<Address>(&d.info);
}

// A loaded asset or exported UI record by name: from the card's own asset domain and the domains
// of mounted presenters, then the exported records of the resource lists that hold them, then the
// blueprints the ReSkate pause page found.
Address asset(const Context &c, const char *name) {
    auto &s = state();
    if (const auto cached = s.assets.find(name); cached != s.assets.end()) return cached->second;
    std::vector<Address> anchors;
    std::set<unsigned> domains;
    const auto add_anchor = [&](Address anchor) {
        if (anchor && domains.insert(read<std::uint16_t>(anchor + 0x16)).second) anchors.push_back(anchor);
    };
    if (!s.mode_cards.empty() && c.type_of(s.mode_cards.front().category.handle) == s.mode_cards.front().category.type)
        if (const auto icon = read<Address>(c.address(c.field(s.mode_cards.front().category, 0x189084da)))) {
            const auto type = read<Address>(icon + 8);
            if (type == c.base + build::native_menu::texture_asset_type || type == c.base + build::native_menu::image_asset_type)
                add_anchor(icon);
        }
    add_anchor(s.anchor_asset);
    for (const auto &root : c.roots({presenter.hash})) {
        const auto blueprint = read<Widget>(root.value).blueprint;
        if (!blueprint || read<Address>(blueprint + 8) != c.base + build::native_menu::widget_blueprint_type) continue;
        add_anchor(blueprint);
        if (domains.size() >= 32) break;
    }
    const auto find_loaded = [&](const char *path) -> Address {
        for (const auto anchor : anchors)
            if (const auto found = named_asset(c, anchor, path)) return found;
        const auto find = game::native_data().find_asset;
        if (find)
            for (unsigned candidate = 2; candidate < 32; ++candidate) {
                auto domain = static_cast<std::uint16_t>(candidate);
                std::set<std::uint16_t> visited;
                bool related = false;
                while (domain > 1 && domain < 0xbbf && visited.size() < 32 && visited.insert(domain).second) {
                    if (domains.contains(domain)) {
                        related = true;
                        break;
                    }
                    const auto owner = read<Address>(c.base + build::engine::domain_owners + domain * 8ULL);
                    if (!owner) break;
                    domain = read<std::uint16_t>(owner + 0x42);
                }
                if (related)
                    if (const auto found = find(static_cast<std::uint16_t>(candidate), path)) return found;
            }
        return 0;
    };
    if (const auto found = find_loaded(name)) {
        s.assets.emplace(name, found);
        return found;
    }
    constexpr std::array containers{
        std::pair{"Activities_DetailsMenu_ContentResource/", "UI/Features/Activities/Resources/Activities_DetailsMenu_ContentResource"},
        std::pair{"StackPanelStylesList/", "UI/Foundations/Components/StackVariations/StackPanel/StackPanelStylesList"},
        std::pair{"TD_Page_ContentResources/", "UI/Features/Throwdowns/TD_Page_ContentResources"},
        std::pair{"TextStylesList/", "UI/Foundations/Styles/Lists/TextStylesList"},
        std::pair{"ButtonStyleList/", "UI/Foundations/Styles/Lists/ButtonStyleList"}};
    for (const auto &[prefix, path] : containers)
        if (std::string_view(name).starts_with(prefix)) {
            const Address list = find_loaded(path);
            if (!list || read<Address>(list + 8) != c.base + build::native_menu::asset_list_type) continue;
            const auto records = read<Address>(list + 0x20);
            const auto count = records ? read<std::uint32_t>(records - 4) & 0x7fffffff : 0;
            require(count <= 2048, "Game modes card: resource list exceeds its limit.");
            for (unsigned i = 0; i < count; ++i) {
                const auto record = read<Address>(records + i * 8ULL);
                if (!record || read<Address>(record + 8) != c.base + build::native_menu::asset_record_type) continue;
                s.assets.emplace(string(read<Address>(record + 0x28), 256), record);
            }
            if (const auto found = s.assets.find(name); found != s.assets.end()) return found->second;
        }
    for (unsigned slot = 0; slot < menu_view::page_count; ++slot) {
        const auto &page_state = native_menu_detail::page_state(slot);
        if (page_state.base != c.base || page_state.manager != c.manager) continue;
        if (const auto found = page_state.assets.find(name);
            found != page_state.assets.end() && (read<Address>(found->second + 8) == c.base + build::native_menu::widget_blueprint_type ||
                                                 read<Address>(found->second + 8) == c.base + build::native_menu::asset_record_type))
            return found->second;
    }
    for (const auto &root : c.roots({core.hash})) {
        try {
            const auto found = blueprints(c, root.model);
            s.assets.insert(found.begin(), found.end());
            if (const auto loaded = s.assets.find(name); loaded != s.assets.end()) return loaded->second;
        } catch (const std::exception &) {}
    }
    throw std::runtime_error(std::string("Game modes card: UI asset unavailable: ") + name);
}
Value record_model(const Context &c, const char *name, Schema schema) {
    const auto record = asset(c, name);
    require(read<Address>(record + 8) == c.base + build::native_menu::asset_record_type, "Game modes card: template is not a native record.");
    const auto type = read<Address>(record + 0x18), data = read<Address>(record + 0x20);
    require(type && data, "Game modes card: template data unavailable.");
    auto result = make(c, schema, type);
    c.copy(result, data);
    return result;
}
Ref record_ref(const Context &c, const char *name) { return {asset(c, name), 0}; }
void clear_value(const Context &c, Value value) {
    if (kind(value.type) == 4) {
        c.array(value, std::span<const std::byte>{}, 0);
        return;
    }
    if (kind(value.type) == 2) {
        const auto defaults =
            make(c, {read<std::uint32_t>(read<Address>(value.type)), static_cast<std::uint16_t>(size(value.type))}, value.type);
        c.copy(value, c.address(defaults));
        return;
    }
    const std::vector<std::byte> empty(size(value.type));
    c.publish(value, empty.data());
}
void plain_label(const Context &c, Value value, const std::string &text) {
    if (c.text(c.field(value, text_field), 2048) != text) c.text(c.field(value, text_field), text);
    c.set(c.field(value, 0x042924a4), true);
}
void label_styles(const Context &c, Value label, const char *idle, const char *focused) {
    c.set(c.field(label, 0xf94d8cc6), record_ref(c, idle));
    c.set(c.field(label, 0xa1e84eb1), record_ref(c, focused));
    c.set(c.field(label, 0xfc23a999), record_ref(c, "TextStylesList/B52-SemiB_Black"));
    c.set(c.field(label, 0x25189231), record_ref(c, focused));
}
// A new anchored presenter or list, copied from an authored record. Not made from nothing: a model
// made from its type alone leaves its callbacks unset (-1), and the game calls them (that crashed
// the game when the first grids were focused).
Value new_anchor(const Context &c) {
    auto anchor = record_model(c, "Activities_DetailsMenu_ContentResource/Activities_Details_AnchoredContentPresenter", anchor_schema);
    // A list item: laid out from the item's start at its own size (the record anchors a page panel).
    for (const auto axis : {0xde7d30c7U, 0x185a5736U}) {
        const auto layout = c.field(anchor, axis);
        c.set(c.field(layout, 0x99bbed5a), 0.f); // AnchorStart
        c.set(c.field(layout, 0x49943bed), 0.f); // AnchorEnd
        c.set(c.field(layout, 0x40bfbff4), 0.f); // OffsetStart
        c.set(c.field(layout, 0x1d233266), 0.f); // OffsetEnd
        c.set(c.field(layout, 0xfec5e0b1), 0.f); // SizingPivot
        c.set(c.field(layout, 0xd01f4a21), 1.f); // SizingWeight
    }
    return anchor;
}
Value new_list(const Context &c) { return record_model(c, "TD_Page_ContentResources/TD_Host_ModeDetailsPanel_Content", linear_list); }
Value compact_anchor(const Context &c, Value model, const char *blueprint, float height, bool focusable) {
    auto result = new_anchor(c);
    c.set(c.field(result, widget), Widget{asset(c, blueprint), {0, model.handle}});
    c.set(c.field(result, 0x1cff7243), std::array<float, 2>{1130.f, height});
    c.set(c.field(result, 0x6efc1a61), false);
    c.set(c.field(result, 0xbe5683d9), false);
    c.set(c.field(result, 0x144aee01), focusable);
    return result;
}

// The authored compact Throwdown creation panel (header with badge, a list body, Back and Confirm in
// the footer), all private copies.
void compact_panel(const Context &c) {
    auto &s = state();
    if (s.panel.handle) return;
    s.panel = record_model(c, "Activities_DetailsMenu_ContentResource/Activities_Details_StackPanel", panel_schema);
    s.panel_anchor = record_model(c, "Activities_DetailsMenu_ContentResource/Activities_Details_AnchoredContentPresenter", anchor_schema);
    c.set(c.field(s.panel_anchor, widget),
          Widget{asset(c, "UI/Foundations/Components/StackVariations/StackPanel/StackPanel_Widget"), {0, s.panel.handle}});
    clear_value(c, c.field(s.panel_anchor, 0x1aa5017f));
    // Centred in the page: anchor start/end, offsets, pivot and weight by name (the stored order
    // differs from the authored schema's).
    for (const auto axis : {0xde7d30c7U, 0x185a5736U}) {
        const auto layout = c.field(s.panel_anchor, axis);
        const bool vertical = axis == 0x185a5736U;
        c.set(c.field(layout, 0x99bbed5a), vertical ? 0.f : .5f);
        c.set(c.field(layout, 0x49943bed), vertical ? 1.f : .5f);
        c.set(c.field(layout, 0x40bfbff4), vertical ? 100.f : 0.f);
        c.set(c.field(layout, 0x1d233266), vertical ? -100.f : 0.f);
        c.set(c.field(layout, 0xfec5e0b1), .5f);
        c.set(c.field(layout, 0xd01f4a21), vertical ? 0.f : 1.f);
    }
    c.set(c.field(s.panel_anchor, 0x6efc1a61), true);
    c.set(c.field(s.panel_anchor, 0xbe5683d9), false);
    s.panel_style = record_model(c, "StackPanelStylesList/StackPanelStyle.ThrowdownCreation", panel_style_schema);
    c.set(c.field(s.panel_style, 0xc13ba637), false);
    c.set(c.field(s.panel, 0x8cc042ef), Ref{0, s.panel_style.handle});
    s.header = record_model(c, "Activities_DetailsMenu_ContentResource/Activities_Details_StackPanel_HeaderContent", header_schema);
    plain_label(c, c.field(s.header, 0x44688629), "Throwdown");
    label_styles(c, c.field(s.header, 0x44688629), "TextStylesList/B52-SemiB_Bright", "TextStylesList/B52-SemiB_Bright");
    plain_label(c, c.field(s.header, 0x00f3b15e), "GAME MODES");
    label_styles(c, c.field(s.header, 0x00f3b15e), "TextStylesList/D180-Caps_White", "TextStylesList/H72-XBold_White");
    c.set(c.path(s.header, {0x00f3b15e, 0xfc53d427}), false);
    c.set(c.field(s.header, 0x2840ecf6), Widget{});
    s.badge = make(c, texture_schema);
    c.copy(c.field(s.badge, 0x6e469d2a), c.address(c.field(s.originals[2].category, 0x189084da)));
    c.set(c.field(s.badge, 0xf524c836), 288.f);
    c.set(c.field(s.badge, 0x6fbd254e), 288.f);
    c.set(c.field(s.badge, 0x81c94d8a), false);
    c.set(c.field(s.badge, 0xd1997d0c), false);
    c.set(c.field(s.badge, 0x190e039c), false);
    c.set(c.field(s.header, content), Widget{asset(c, "UI/Foundations/Components/Media/Icons/Texture_Widget"), {0, s.badge.handle}});
    c.set(c.field(s.panel, 0xc0f1b449),
          Widget{asset(c, "UI/Foundations/Components/ContentPresenterVariations/ContentWithTitle_Widget"), {0, s.header.handle}});
    s.panel_notice = record_model(c, "TD_Page_ContentResources/TD_Notice_MenuDescriptionTitle", notice_schema);
    plain_label(c, c.field(s.panel_notice, 0x94703efc),
                "ReSkate game modes. Set one up and everyone in your lobby with game modes gets an invite.");
    c.set(c.field(s.panel_notice, 0x13be0d51), std::array<float, 2>{1120.f, 160.f});
    label_styles(c, c.field(s.panel_notice, 0x94703efc), "TextStylesList/B52-SemiB_Bright", "TextStylesList/B52-SemiB_Bright");
    c.set(c.field(s.panel, 0x4fd7853d), Widget{});
    auto &rules = s.rows["rules"];
    rules.model = s.panel_notice;
    rules.anchor = compact_anchor(c, s.panel_notice, "UI/Foundations/Components/Notices/BasicNotice_Widget", 160.f, false);
    // The body: the authored mode-details list, emptied, vertical, scrolling.
    s.details_list = record_model(c, "TD_Page_ContentResources/TD_Host_ModeDetailsPanel_Content", linear_list);
    c.array(c.field(s.details_list, rows_field), std::vector<Ref>{});
    c.set(c.field(s.details_list, 0x55ca89f4), asset(c, "UI/Foundations/Components/Lists/Shared/AnchoredContentPresenterListItem_Widget"));
    c.set(c.field(s.details_list, 0x4cb61cac), 1);
    c.set(c.field(s.details_list, 0x64b9a9e8), 0);
    c.set(c.field(s.details_list, 0x19ff199a), 130.f);
    c.set(c.field(s.details_list, 0xebca7354), 12.f);
    c.set(c.field(s.details_list, 0xc33d3081), true);
    c.set(c.field(s.details_list, 0x4554761c), true);
    c.set(c.field(s.details_list, 0xafe8434b), false);
    c.set(c.field(s.details_list, 0x8bbf7d67), true);
    c.set(c.field(s.details_list, 0xce0ef621), false);
    c.set(c.field(s.details_list, 0xa764b17a), false);
    c.set(c.field(s.details_list, 0x0e0fdf9f), false);
    auto first = make(c, stack_item);
    c.set(c.field(first, 0x1e95752c), static_cast<int>(0x524d4f10U));
    c.copy(c.field(first, 0xd78d240b), c.address(c.path(s.panel, {0x43967ece, 0x54c9abee})));
    c.set(c.field(first, content),
          Widget{asset(c, "UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"), {0, s.details_list.handle}});
    c.set(c.field(first, 0x369babfe), true);
    // A fresh stack item starts collapsed (progress zero); the body is always shown.
    for (const auto track : {0x8c0583eeU, 0xc5c7cc11U}) {
        c.set(c.path(first, {track, 0x20247753}), 1.f);
        c.set(c.path(first, {track, 0x2426103f}), 1.f);
        c.set(c.path(first, {track, 0x1e3ce013}), true);
    }
    std::vector<std::byte> item_bytes(stack_item.size);
    require(memory::peek_bytes(c.address(first), item_bytes.data(), stack_item.size), "Game modes card: panel item unavailable.");
    c.array(c.path(s.panel, {0x43967ece, items}), item_bytes, 1);
    c.set(c.path(s.panel, {0x43967ece, 0x1fbbb5da}), true);
    c.set(c.path(s.panel, {0x43967ece, 0x369babfe}), true);
    c.set(c.path(s.panel, {0x43967ece, 0xc912aa1d}), false);
    c.set(c.path(s.panel, {0x43967ece, 0x2a98baa3}), false);
    c.set(c.path(s.panel, {0x43967ece, 0x18f8355b}), -1);
    s.footer_anchor =
        record_model(c, "Activities_DetailsMenu_ContentResource/Activities_Details_StackPanel_Footer_AnchoredContentPresenter", anchor_schema);
    s.footer_list = record_model(c, "Activities_DetailsMenu_ContentResource/Activities_Details_StackPanel_FooterContent", linear_list);
    s.footer_back = record_model(c, "TD_Page_ContentResources/TD_Back_Button", button);
    s.footer_confirm = record_model(c, "TD_Page_ContentResources/TD_Create_Button", button);
    for (const auto b : {s.footer_back, s.footer_confirm}) {
        c.copy(c.field(b, label_field), c.address(s.source_label));
        label_styles(c, c.field(b, label_field), "TextStylesList/B52-SemiB-Caps_Dark", "TextStylesList/B52-SemiB_Black");
        c.set(c.path(b, {label_field, 0x3d8639d0}), 2);
        c.set(c.field(b, 0x659db23e), false);
        clear_value(c, c.field(b, 0x67212c85)); // the stock mode's queue predicates
    }
    c.set(c.path(s.footer_back, {0x0fb0d794, 0x8cc042ef}), record_ref(c, "ButtonStyleList/ButtonStyle.CTA.Default"));
    c.set(c.path(s.footer_confirm, {0x0fb0d794, 0x8cc042ef}), record_ref(c, "ButtonStyleList/ButtonStyle.CTA.Hero"));
    c.array(c.field(s.footer_list, rows_field), std::vector<Ref>{{0, s.footer_back.handle}, {0, s.footer_confirm.handle}});
    c.set(c.field(s.footer_anchor, widget),
          Widget{asset(c, "UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"), {0, s.footer_list.handle}});
    c.set(c.field(s.panel, 0xbfc64535),
          Widget{asset(c, "UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget"), {0, s.footer_anchor.handle}});
    // Confirm as a page action, on the primary input, as the stock creation page has it.
    const auto action_type = read<Address>(read<Address>(c.field(s.page, page_actions).type) + 0x30);
    s.confirm_action = make(c, page_action_schema, action_type);
    c.copy(c.field(s.confirm_action, 0xa704272a), c.address(c.path(s.footer_confirm, {0x0fb0d794, 0xa704272a})));
    s.primary_input = 0;
    const auto native_actions = c.field(s.original_actions, page_actions);
    unsigned action_count{}, action_stride{};
    c.array(native_actions, 8, action_count, action_stride);
    require(action_stride == page_action_schema.size, "Game modes card: page action schema differs.");
    for (unsigned i = 0; i < action_count && !s.primary_input; ++i) {
        const auto original = c.path(c.element(native_actions, i), {0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
        const auto candidate = read<Address>(c.address(c.field(original, 0x3d00de27)));
        if (candidate && string(read<Address>(candidate + 0x18)) == "Configurations/Input/UI/UI_PrimaryAction") s.primary_input = candidate;
    }
    if (!s.primary_input) s.primary_input = asset(c, "Configurations/Input/UI/UI_PrimaryAction");
    logging::write(logging::Level::info, logging::Channel::ui, "Game modes card: native mode panel built.");
}
Value category(const Context &c, Value tile) {
    const auto w = read<Widget>(c.address(c.path(tile, {widget, widget})));
    const auto model = reference(c, w.data);
    require(model.type && size(model.type) == category_schema.size && read<std::uint32_t>(read<Address>(model.type)) == category_schema.hash,
            "Game modes card: category schema differs.");
    return model;
}
Value description(const Context &c, Value cat) { return reference(c, read<Widget>(c.address(c.field(cat, widget))).data); }
// The Throwdowns page's card list (three tiles whose buttons name the stock modes) and the
// presenter showing it.
struct CardList {
    Value list, holder;
};
std::optional<CardList> card_list(const Context &c, Value p) {
    try {
        auto holder = body(c, p);
        auto w = read<Widget>(c.address(holder));
        for (unsigned depth = 0; depth < 4; ++depth) {
            auto value = reference(c, w.data);
            if (!value.type) return {};
            const auto hash = read<std::uint32_t>(read<Address>(value.type));
            if (hash == linear_list.hash && size(value.type) == linear_list.size) {
                unsigned count{}, stride{};
                auto bytes = c.array(c.field(value, rows_field), 4, count, stride);
                if (count != 3 || stride != sizeof(Ref)) return {};
                // Skate Jam's card navigates through ThrowdownerActive; JamSession is its mode.
                const std::array<std::string_view, 3> expected{"ThrowdownSkate", "SpotBattle", "ThrowdownerActive"};
                for (unsigned i = 0; i < 3; ++i) {
                    Ref ref{};
                    std::memcpy(&ref, bytes.data() + i * stride, stride);
                    const auto t = reference(c, ref);
                    if (!t.type || size(t.type) != tile_schema.size || read<std::uint32_t>(read<Address>(t.type)) != tile_schema.hash ||
                        c.text(c.field(primary(c, t), navigation_field)) != expected[i])
                        return {};
                }
                return CardList{value, holder};
            }
            if (hash != anchor_schema.hash || size(value.type) != anchor_schema.size) return {};
            holder = c.field(value, widget);
            w = read<Widget>(c.address(holder));
        }
    } catch (...) {}
    return {};
}
void switch_page(const Context &c, bool details) {
    auto &s = state();
    if (details) {
        compact_panel(c);
        c.set(body(c, s.page), Widget{asset(c, "UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget"), {0, s.panel_anchor.handle}});
        c.set(c.field(back(c, s.page), callback_field), callback(c, "back"));
        c.text(c.field(back(c, s.page), navigation_field), "");
        c.text(c.path(s.page, {title_path, text_field}), "");
    } else {
        c.copy(body(c, s.page), c.address(s.original_body));
        c.copy(back(c, s.page), c.address(s.original_back));
        c.copy(c.field(s.page, page_actions), c.address(c.field(s.original_actions, page_actions)));
        c.text(c.path(s.page, {title_path, text_field}), s.original_title);
        s.displayed.clear();
    }
    s.details = details;
}
// Shows a card at the shared smaller size (its authored sizes are `o`'s), publishing only changes.
void shrink(const Context &c, Value tile, Value cat, Value desc, const Original &o, CardScale k) {
    const std::array<float, 2> size{o.tile_size[0] * k.width, o.tile_size[1] * k.height};
    if (read<std::array<float, 2>>(c.address(c.field(tile, 0x868043e1))) != size) c.set(c.field(tile, 0x868043e1), size);
    const std::array<float, 2> text{o.description_size[0] * k.width, o.description_size[1] * k.height};
    if (read<std::array<float, 2>>(c.address(c.field(desc, 0x1cff7243))) != text) c.set(c.field(desc, 0x1cff7243), text);
    for (const auto [hash, value] : std::array{std::pair{0x495ffd43U, o.icon_width * k.icon}, std::pair{0x24fb5ca8U, o.icon_height * k.icon}})
        if (read<float>(c.address(c.field(cat, hash))) != value) c.set(c.field(cat, hash), value);
}
void shrink_all(const Context &c) {
    auto &s = state();
    const auto k = s.grid.handle ? grid_scale : row_scale;
    for (const auto &o : s.originals) shrink(c, o.tile, o.category, o.description, o, k);
    for (const auto &card : s.mode_cards) shrink(c, card.tile, card.category, card.description, s.originals[1], k);
}
bool ours(const State &s, Handle handle) {
    return std::any_of(s.mode_cards.begin(), s.mode_cards.end(), [&](const ModeCard &card) { return card.tile.handle == handle; });
}
void release(const Context &c) {
    auto &s = state();
    s.owner.store(0);
    {
        std::lock_guard lock(s.mutex);
        s.pending.clear();
    }
    s.owned.release(
        [&] {
            if (s.page.handle && c.type_of(s.page.handle) == s.page.type && s.details) switch_page(c, false);
            // The stock row back where the grid was, with its cards' descriptions.
            if (s.grid.handle && s.holder.handle && c.type_of(s.holder.handle) == s.holder.type &&
                read<Widget>(c.address(s.holder)).data.handle == s.grid.handle)
                c.set(s.holder, s.original_holder);
            if (s.grid.handle)
                for (const auto &o : s.originals)
                    if (o.label.handle && c.type_of(o.label.handle) == o.label.type) c.text(c.field(o.label, text_field), o.text);
            if (s.cards.handle && c.type_of(s.cards.handle) == s.cards.type) {
                unsigned count{}, stride{};
                auto bytes = c.array(c.field(s.cards, rows_field), 32, count, stride);
                require(stride == sizeof(Ref), "Game modes card: card list changed during cleanup.");
                std::vector<Ref> retained;
                for (unsigned i = 0; i < count; ++i) {
                    Ref ref{};
                    std::memcpy(&ref, bytes.data() + i * stride, stride);
                    if (!ours(s, ref.handle)) retained.push_back(ref);
                }
                c.array(c.field(s.cards, rows_field), retained);
                c.set(c.field(s.cards, clip_field), s.original_clip);
            }
            for (const auto &o : s.originals) {
                if (c.type_of(o.tile.handle) == o.tile.type) c.set(c.field(o.tile, 0x868043e1), o.tile_size);
                if (c.type_of(o.category.handle) == o.category.type) {
                    c.set(c.field(o.category, 0x495ffd43), o.icon_width);
                    c.set(c.field(o.category, 0x24fb5ca8), o.icon_height);
                }
                if (c.type_of(o.description.handle) == o.description.type) c.set(c.field(o.description, 0x1cff7243), o.description_size);
            }
        },
        [&](Value value) { c.destroy(value); });
    reset(false);
}
// A card for `mode`: a private copy of Spot Battle's tile with the mode's name, description and a
// stock card's art. Its button calls back with the mode (the native navigation id is cleared).
ModeCard mode_card(const Context &c, modes::Mode mode) {
    auto &s = state();
    const auto &donor = s.originals[1];
    ModeCard card{mode, std::string(modes::mode_key(mode)), upper(modes::mode_name(mode))};
    auto blurb = std::string(modes::mode_tagline(mode));
    if (blurb.empty()) blurb = modes::mode_summary(mode);
    const auto card_widget = read<Widget>(c.address(c.path(donor.tile, {widget, widget})));
    card.tile = make(c, tile_schema);
    c.copy(card.tile, c.address(donor.tile));
    card.category = make(c, category_schema);
    c.copy(card.category, c.address(donor.category));
    for (const auto hash : {0x189084daU, 0xc2917efcU})
        c.copy(c.field(card.category, hash), c.address(c.field(s.originals[card_art(mode)].category, hash)));
    card.description = make(c, anchor_schema);
    c.copy(card.description, c.address(donor.description));
    card.label = make(c, label_schema);
    c.copy(card.label, c.address(s.source_label));
    c.text(c.field(card.label, text_field), blurb);
    c.set(c.field(card.label, 0x042924a4), true);
    auto desc_widget = read<Widget>(c.address(c.field(card.description, widget)));
    desc_widget.data = {0, card.label.handle};
    c.set(c.field(card.description, widget), desc_widget);
    auto cat_widget = read<Widget>(c.address(c.field(card.category, widget)));
    cat_widget.data = {0, card.description.handle};
    c.set(c.field(card.category, widget), cat_widget);
    c.text(c.path(card.category, {0x77b8ed0b, text_field}), card.name);
    c.set(c.path(card.category, {0x77b8ed0b, 0x042924a4}), true);
    auto cw = card_widget;
    cw.data = {0, card.category.handle};
    c.set(c.path(card.tile, {widget, widget}), cw);
    c.text(c.field(primary(c, card.tile), navigation_field), "");
    c.set(c.field(primary(c, card.tile), callback_field), callback(c, "card", card.key));
    return card;
}
// An asset's name (widget blueprints and other named assets keep it at +0x18), for the log.
std::string asset_name(Address asset) {
    try {
        return asset ? string(read<Address>(asset + 0x18), 256) : std::string("none");
    } catch (...) {
        return "?";
    }
}
// What the stock card row is, for diagnosing the grid.
void describe_row(const Context &c) {
    auto &s = state();
    try {
        unsigned count{}, stride{};
        c.array(c.field(s.cards, handles_field), 64, count, stride);
        logging::log(logging::Level::info, logging::Channel::ui,
                     "Game modes card: stock row widget {}, item widget {}, orientation {}, item size {}, spacing {}, sizing {}, "
                     "clip {}, mounted items {}.",
                     asset_name(s.original_holder.blueprint), asset_name(read<Address>(c.address(c.field(s.cards, 0x55ca89f4)))),
                     read<std::int32_t>(c.address(c.field(s.cards, 0x4cb61cac))), read<float>(c.address(c.field(s.cards, 0x19ff199a))),
                     read<float>(c.address(c.field(s.cards, 0xebca7354))), read<std::int32_t>(c.address(c.field(s.cards, 0x64b9a9e8))),
                     read<bool>(c.address(c.field(s.cards, clip_field))), count);
    } catch (const std::exception &e) {
        logging::log(logging::Level::info, logging::Channel::ui, "Game modes card: stock row not described ({}).", e.what());
    }
}
// The grid: every card (the three stock ones first) in rows of `cards_per_row`. Each row is a new
// list (not a copy of the stock row: a native copy shares the stock row's own models, and the
// first grid, built that way, crashed the game) with the stock row's item widget and spacing; the
// grid is a vertical list of the rows' presenters, clipped so it scrolls to the focused row.
// Shown where the stock row was.
void build_grid(const Context &c) {
    auto &s = state();
    std::vector<Ref> tiles;
    for (const auto &o : s.originals) tiles.push_back({0, o.tile.handle});
    for (const auto &card : s.mode_cards) tiles.push_back({0, card.tile.handle});
    const auto &sized = s.originals[1];
    const float tile_w = sized.tile_size[0] * grid_scale.width, tile_h = sized.tile_size[1] * grid_scale.height;
    const float row_w = static_cast<float>(cards_per_row) * tile_w + static_cast<float>(cards_per_row - 1) * card_gap;
    const auto list_widget = asset(c, "UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget");
    const auto tile_widget = read<Address>(c.address(c.field(s.cards, 0x55ca89f4)));
    require(tile_widget, "Game modes card: the stock row has no item widget.");
    // Names and logos only: every card's description is blanked (the stock ones' put back on release).
    for (const auto &o : s.originals)
        if (o.label.type && size(o.label.type) == label_schema.size) c.text(c.field(o.label, text_field), "");
    for (const auto &card : s.mode_cards) c.text(c.field(card.label, text_field), "");
    std::vector<Handle> anchors;
    // An empty first row keeps the grid clear of the page's title.
    {
        auto blank = make(c, label_schema);
        c.copy(blank, c.address(s.source_label));
        c.text(c.field(blank, text_field), "");
        auto spacer = new_anchor(c);
        c.set(c.field(spacer, widget), Widget{asset(c, "UI/Foundations/Components/Text/Label/Widget/Label_Widget"), {0, blank.handle}});
        c.set(c.field(spacer, 0x1cff7243), std::array<float, 2>{row_w, grid_top});
        c.set(c.field(spacer, 0x6efc1a61), false);
        c.set(c.field(spacer, 0xbe5683d9), false);
        c.set(c.field(spacer, 0x144aee01), false);
        anchors.push_back(spacer.handle);
    }
    for (std::size_t first = 0; first < tiles.size(); first += cards_per_row) {
        auto row = new_list(c);
        c.array(c.field(row, handles_field), std::vector<Handle>{});
        c.set(c.field(row, 0x55ca89f4), tile_widget);
        c.set(c.field(row, 0x4cb61cac), 0); // Orientation: horizontal
        c.set(c.field(row, 0x64b9a9e8), read<std::int32_t>(c.address(c.field(s.cards, 0x64b9a9e8))));
        c.set(c.field(row, 0x19ff199a), read<float>(c.address(c.field(s.cards, 0x19ff199a))));
        c.set(c.field(row, 0xebca7354), card_gap);
        c.set(c.field(row, clip_field), false);
        c.array(c.field(row, rows_field),
                std::vector<Ref>(tiles.begin() + static_cast<std::ptrdiff_t>(first),
                                 tiles.begin() + static_cast<std::ptrdiff_t>(std::min(tiles.size(), first + cards_per_row))));
        auto anchor = new_anchor(c);
        c.set(c.field(anchor, widget), Widget{list_widget, {0, row.handle}});
        c.set(c.field(anchor, 0x1cff7243), std::array<float, 2>{row_w, tile_h});
        c.set(c.field(anchor, 0x6efc1a61), false);
        c.set(c.field(anchor, 0xbe5683d9), false);
        c.set(c.field(anchor, 0x144aee01), true);
        anchors.push_back(anchor.handle);
    }
    auto grid = new_list(c);
    c.array(c.field(grid, rows_field), std::vector<Ref>{});
    c.set(c.field(grid, 0x55ca89f4), asset(c, "UI/Foundations/Components/Lists/Shared/AnchoredContentPresenterListItem_Widget"));
    c.set(c.field(grid, 0x4cb61cac), 1);       // Orientation: vertical
    c.set(c.field(grid, 0x64b9a9e8), 0);       // the rows' own measures
    c.set(c.field(grid, 0x19ff199a), tile_h);  // row height
    c.set(c.field(grid, 0xebca7354), card_gap);
    c.set(c.field(grid, clip_field), true);    // EnableClipping: scrolls to the focused row
    c.set(c.field(grid, 0xafe8434b), false);
    c.set(c.field(grid, 0xc33d3081), true);    // RememberFocusedIndex
    c.array(c.field(grid, handles_field), anchors);
    c.set(c.field(grid, focus_field), 1); // the first row of cards, not the spacer
    s.grid = grid;
    logging::log(logging::Level::info, logging::Channel::ui, "Game modes card: {} cards in a grid of {} rows.", tiles.size(), anchors.size());
}
// The grid where the stock row is shown, unless the mode panel is up.
void mount_grid(const Context &c) {
    auto &s = state();
    if (s.details || !s.grid.handle || c.type_of(s.holder.handle) != s.holder.type) return;
    if (read<Widget>(c.address(s.holder)).data.handle != s.grid.handle)
        c.set(s.holder, Widget{asset(c, "UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"), {0, s.grid.handle}});
}
// Without a grid: the three stock cards, then ours, in the stock row, which clips to its bounds so
// it scrolls to the focused card.
void publish_cards(const Context &c) {
    auto &s = state();
    unsigned count{}, stride{};
    const auto bytes = c.array(c.field(s.cards, rows_field), 32, count, stride);
    require(stride == sizeof(Ref), "Game modes card: card list changed.");
    std::vector<Ref> refs(count);
    if (count) std::memcpy(refs.data(), bytes.data(), bytes.size());
    std::vector<Ref> wanted;
    for (const auto &ref : refs)
        if (!ours(s, ref.handle)) wanted.push_back(ref);
    require(wanted.size() == 3, "Game modes card: card list was replaced.");
    for (const auto &card : s.mode_cards) wanted.push_back({0, card.tile.handle});
    const bool same = refs.size() == wanted.size() && std::equal(refs.begin(), refs.end(), wanted.begin(), [](const Ref &a, const Ref &b) {
        return a.handle == b.handle && a.record == b.record;
    });
    if (!same) c.array(c.field(s.cards, rows_field), wanted);
    if (!read<bool>(c.address(c.field(s.cards, clip_field)))) c.set(c.field(s.cards, clip_field), true);
}
void initialize(const Context &c, Value p, Value list, Value holder) {
    auto &s = state();
    s.page = p;
    s.cards = list;
    s.owner.store(p.handle);
    s.next_id = 0;
    unsigned count{}, stride{};
    auto bytes = c.array(c.field(list, rows_field), 3, count, stride);
    std::vector<Ref> refs(count);
    std::memcpy(refs.data(), bytes.data(), bytes.size());
    for (const auto &ref : refs) {
        auto tile = reference(c, ref), cat = category(c, tile), desc = description(c, cat);
        const auto label = reference(c, read<Widget>(c.address(c.field(desc, widget))).data);
        s.originals.push_back({tile, cat, desc, read<std::array<float, 2>>(c.address(c.field(tile, 0x868043e1))),
                               read<std::array<float, 2>>(c.address(c.field(desc, 0x1cff7243))), read<float>(c.address(c.field(cat, 0x495ffd43))),
                               read<float>(c.address(c.field(cat, 0x24fb5ca8))), label,
                               label.type && size(label.type) == label_schema.size ? c.text(c.field(label, text_field), 2048) : std::string()});
    }
    const auto &donor = s.originals[1];
    s.anchor_asset = read<Widget>(c.address(c.path(donor.tile, {widget, widget}))).blueprint;
    s.source_label = reference(c, read<Widget>(c.address(c.field(donor.description, widget))).data);
    require(s.source_label.type && size(s.source_label.type) == label_schema.size, "Game modes card: description label unavailable.");
    // One card per registered mode, each once; a mode without a key or name gets none (and says so).
    std::set<std::string> keys;
    for (const auto mode : modes::card_order) {
        // skate.'s own S.K.A.T.E. card is on the page already; ours is in the ReSkate menu.
        if (mode == modes::Mode::skate) continue;
        const auto key = modes::mode_key(mode);
        if (key.empty() || modes::mode_name(mode).empty() || !keys.insert(std::string(key)).second) {
            logging::log(logging::Level::warning, logging::Channel::ui, "Game modes card: mode {} has no usable key or name; no card.",
                         static_cast<int>(mode));
            continue;
        }
        s.mode_cards.push_back(mode_card(c, mode));
    }
    s.original_body = make(c, presenter);
    c.copy(s.original_body, c.address(body(c, p)));
    s.original_back = make(c, action_schema);
    c.copy(s.original_back, c.address(back(c, p)));
    s.original_title = c.text(c.path(p, {title_path, text_field}));
    s.original_actions = make(c, page);
    c.copy(c.field(s.original_actions, page_actions), c.address(c.field(p, page_actions)));
    s.original_clip = read<bool>(c.address(c.field(list, clip_field)));
    s.holder = holder;
    s.original_holder = read<Widget>(c.address(holder));
    describe_row(c);
    if (modes::throwdown_grid()) {
        try {
            build_grid(c);
        } catch (const std::exception &e) {
            // A grid that cannot be built leaves the stock row, with our cards added to it.
            logging::log(logging::Level::warning, logging::Channel::ui, "Game modes card: no grid ({}); cards in a row.", e.what());
            s.grid = {};
        }
    }
    shrink_all(c);
    if (s.grid.handle) mount_grid(c);
    else publish_cards(c);
    s.generations.emplace(p.handle, list.handle);
    logging::log(logging::Level::info, logging::Channel::ui, "Game modes card: {} mode cards added to the Throwdowns menu.", s.mode_cards.size());
}

// One row of the mode panel: a button (with a command) or a plain label.
void row(const Context &c, std::vector<std::string> &shown, const std::string &id, const std::string &text, std::string command = {},
         std::string argument = {}) {
    auto &s = state();
    auto [entry, fresh] = s.rows.try_emplace(id);
    auto &r = entry->second;
    const bool button_row = !command.empty();
    if (fresh) {
        r.model = make(c, button_row ? button : label_schema);
        r.anchor = new_anchor(c);
        auto l = button_row ? c.field(r.model, label_field) : r.model;
        c.copy(l, c.address(s.source_label));
        c.set(c.field(l, 0x042924a4), true);
        c.set(c.field(r.anchor, widget),
              Widget{asset(c, button_row ? "UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget"
                                         : "UI/Foundations/Components/Text/Label/Widget/Label_Widget"),
                     {0, r.model.handle}});
        c.set(c.field(r.anchor, 0x1cff7243), std::array<float, 2>{1130.f, button_row ? 120.f : 90.f});
        c.set(c.field(r.anchor, 0x6efc1a61), false);
        c.set(c.field(r.anchor, 0xbe5683d9), false);
        c.set(c.field(r.anchor, 0x144aee01), button_row);
    }
    auto l = button_row ? c.field(r.model, label_field) : r.model;
    if (c.text(c.field(l, text_field), 2048) != text) c.text(c.field(l, text_field), text);
    r.text = text;
    // Button constructors can restore their defaults after mounting: keep the styles readable.
    c.set(c.field(l, 0x042924a4), true);
    c.set(c.field(l, 0xfc53d427), false);
    for (const auto hash : {0xf94d8cc6U, 0xa1e84eb1U, 0xfc23a999U, 0x25189231U}) {
        const auto wanted = read<Ref>(c.address(c.field(s.source_label, button_row ? hash : 0xf94d8cc6U)));
        const auto current = read<Ref>(c.address(c.field(l, hash)));
        if (current.record != wanted.record || current.handle != wanted.handle) c.set(c.field(l, hash), wanted);
    }
    if (button_row) {
        c.set(c.field(r.model, 0x659db23e), false);
        c.set(c.field(l, 0x3d8639d0), 0);
        const auto &native_page = native_menu_detail::page_state(0);
        if (native_page.manager == c.manager && native_page.menu_style.handle && c.type_of(native_page.menu_style.handle) == native_page.menu_style.type)
            c.set(c.path(r.model, {0x0fb0d794, 0x8cc042ef}), Ref{0, native_page.menu_style.handle});
        const auto at = c.path(r.model, {0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
        const auto action = callback(c, std::move(command), std::move(argument));
        if (r.callback != action) {
            c.set(c.field(at, callback_field), action);
            c.text(c.field(at, navigation_field), "");
            r.callback = action;
        }
    } else {
        c.set(c.field(l, 0xcd71a279), false);
        c.set(c.field(l, 0x3fa0b887), false);
    }
    shown.push_back(id);
}
void footer_action(const Context &c, Value button_value, const std::string &text, std::string command, std::string argument = {}) {
    auto &s = state();
    plain_label(c, c.field(button_value, label_field), text);
    auto action = c.path(button_value, {0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
    c.set(c.field(action, callback_field), callback(c, std::move(command), std::move(argument)));
    c.text(c.field(action, navigation_field), "");
    if (button_value.handle != s.footer_confirm.handle) return;
    // Confirm also answers the primary input from anywhere on the page, like the stock one.
    auto page_action = c.path(s.confirm_action, {0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
    c.copy(page_action, c.address(action));
    c.set(c.field(page_action, 0x3d00de27), s.primary_input);
    c.set(c.field(page_action, 0x0169c2db), s.primary_input);
    const auto actions = c.field(s.page, page_actions);
    unsigned count{}, stride{};
    c.array(actions, 8, count, stride);
    require(stride == page_action_schema.size, "Game modes card: page action schema differs.");
    bool publish = count != 1;
    if (!publish) {
        const auto mounted = c.path(c.element(actions, 0), {0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
        publish = read<Address>(c.address(c.field(mounted, callback_field))) != read<Address>(c.address(c.field(page_action, callback_field)));
    }
    if (publish) {
        std::vector<std::byte> bytes(page_action_schema.size);
        require(memory::peek_bytes(c.address(s.confirm_action), bytes.data(), bytes.size()), "Game modes card: Confirm action unavailable.");
        c.array(actions, bytes, 1);
    }
}
std::string mode_title(const std::string &key) {
    const auto mode = modes::parse_mode(key);
    return mode ? upper(modes::mode_name(*mode)) : std::string("GAME");
}
// The panel's rows from the game modes' menu snapshot.
void render(const Context &c) {
    auto &s = state();
    const auto m = modes::menu_view();
    std::vector<std::string> shown{"rules"};
    const auto note = [&](const std::string &id, const std::string &text) { row(c, shown, id, text); };
    const auto action = [&](const std::string &id, const std::string &text, std::string verb) { row(c, shown, id, text, "mode", std::move(verb)); };
    footer_action(c, s.footer_back, "BACK", "back");
    if (!m.in_game) {
        for (const auto &offer : m.offers)
            if (!offer.own && offer.open)
                action("offer-" + std::to_string(offer.id), std::format("JOIN {}'S {}", offer.host, offer.mode), "join " + std::to_string(offer.id));
        for (const auto &card : s.mode_cards) action("new-" + card.key, card.name, "new " + card.key);
        footer_action(c, s.footer_confirm, "CLOSE", "back");
    } else {
        const char *phase = m.phase == 1 ? "setting up" : m.phase == 2 ? "starting" : m.phase == 3 ? "in progress" : "finished";
        note("game", std::format("{} - {} - {} player{}", mode_title(m.mode), phase, m.players, m.players == 1 ? "" : "s"));
        if (m.leading && m.phase == 1) {
            if (!m.missing.empty()) note("missing", m.missing);
            const auto mode = modes::parse_mode(m.mode);
            const bool wide = mode && modes::tag_like(*mode);
            const bool race = mode == modes::Mode::race, spots = mode == modes::Mode::domination;
            // What is placed so far, then the free-camera placing (it closes Throwdowns first).
            if (race)
                note("route", m.points < 2 ? std::string("Route: not placed yet")
                                           : std::format("Route: start, {} checkpoint{}, finish", m.points - 2, m.points == 3 ? "" : "s"));
            else
                note("area", m.area_radius > 0 ? std::format("Play area: circle {:.0f} m across", m.area_radius * 2)
                             : m.corners >= 3  ? std::format("Play area: {} corners", m.corners)
                                               : std::string("Play area: the whole map"));
            if (spots) note("spots", std::format("Spots placed: {}", m.points));
            action("start", "START GAME", "start");
            if (race) row(c, shown, "place-route", "PLACE ROUTE AND CHECKPOINTS", "place", "points");
            if (spots) row(c, shown, "place-spots", "PLACE SPOTS", "place", "points");
            if (!race) {
                row(c, shown, "place-circle", "PLACE PLAY AREA", "place", "circle");
                action("circle", wide ? "PLAY AREA: 150 M AROUND ME" : "PLAY AREA: 40 M AROUND ME", wide ? "circle 150" : "circle 40");
            }
            action("stop", "CANCEL GAME", "stop");
            footer_action(c, s.footer_confirm, "START GAME", "mode", "start");
        } else if (m.leading) {
            action("stop", "END GAME FOR EVERYONE", "stop");
            footer_action(c, s.footer_confirm, "END GAME", "mode", "stop");
        } else {
            action("leave", "LEAVE GAME", "leave");
            footer_action(c, s.footer_confirm, "LEAVE GAME", "mode", "leave");
        }
        note("more", "More settings: GAME MODES in the ReSkate menu (Insert).");
    }
    if (!s.status.empty()) note("status", s.status);
    // Footer: Back and Confirm, reconciled after native mounting clears the list.
    const auto mounted = read<Address>(c.address(c.field(s.footer_list, handles_field)));
    const auto mounted_count = mounted ? read<std::uint32_t>(mounted - 4) & 0x7fffffff : 0;
    if (mounted_count != 2) {
        c.array(c.field(s.footer_list, rows_field), std::vector<Ref>{});
        c.array(c.field(s.footer_list, handles_field), std::vector<Handle>{s.footer_back.handle, s.footer_confirm.handle});
    }
    const auto current = read<Address>(c.address(c.field(s.details_list, handles_field)));
    const auto count = current ? read<std::uint32_t>(current - 4) & 0x7fffffff : 0;
    if (shown != s.displayed || count != shown.size()) {
        const auto old_focus = read<int>(c.address(c.field(s.details_list, focus_field)));
        const auto focused = old_focus >= 0 && static_cast<std::size_t>(old_focus) < s.displayed.size() ? s.displayed[old_focus] : std::string{};
        std::vector<Handle> handles;
        int first = -1, preserved = -1;
        for (const auto &id : shown) {
            const auto &r = s.rows.at(id);
            if (r.callback) {
                if (first < 0) first = static_cast<int>(handles.size());
                if (id == focused) preserved = static_cast<int>(handles.size());
            }
            handles.push_back(r.anchor.handle);
        }
        c.array(c.field(s.details_list, handles_field), handles);
        c.set(c.field(s.details_list, focus_field), preserved >= 0 ? preserved : first);
        s.displayed = std::move(shown);
    }
}
// A native screen change, as the game's own buttons queue it: the StateNavigationModel's current
// command (a name hash) with no arguments (metadata [-1]). Nothing is sent over a change the game
// has queued and not run yet; the Back stack is left alone.
bool navigate(const Context &c, std::string_view name) {
    const auto roots = c.roots({0xcc4776b5});
    if (roots.size() != 1) return false;
    const auto current = c.field(roots.front().model, 0xf3ba9cf8);
    const auto metadata = c.field(roots.front().model, 0xf437b255);
    if (read<std::uint32_t>(c.address(current))) return false;
    c.set(current, game::native_name_hash(name));
    c.array(metadata, std::vector<std::int32_t>{-1});
    logging::log(logging::Level::info, logging::Channel::ui, "Game modes card: navigation {}.", name);
    return true;
}
void forget(Address manager) {
    auto &s = state();
    s.owned.manager_replaced();
    reset(true);
    s.manager = manager;
}
} // namespace

namespace {
bool release_now(std::uintptr_t base, bool generations_too) noexcept;
}
bool release_native_modes_card(std::uintptr_t base) noexcept { return release_now(base, true); }
namespace {
// After a failure the same page generation is left alone (`generations_too` false), so a card
// that cannot be built is not rebuilt every few seconds.
bool release_now(std::uintptr_t base, bool generations_too) noexcept {
    auto &s = state();
    if (s.owned.empty()) {
        s.owner.store(0);
        if (generations_too) s.generations.clear();
        return true;
    }
    try {
        const auto ui = read<Address>(base + build::engine::ui_manager);
        const auto manager = ui ? read<Address>(ui + 0x140) : 0;
        require(manager != 0, "Game modes card: UI manager unavailable during cleanup.");
        if (manager != s.manager) {
            forget(manager);
            return true;
        }
        const Context c(base, manager);
        game::ModelWriteLock lock(manager);
        release(c);
        if (generations_too) s.generations.clear();
        return true;
    } catch (const std::exception &e) {
        logging::log(logging::Level::warning, logging::Channel::ui, "Game modes card cleanup: {}", e.what());
        return false;
    }
}
} // namespace

void tick_native_modes_card(std::uintptr_t base, bool loading) noexcept {
    auto &s = state();
    const auto now = GetTickCount64();
    if (loading || now < s.next_tick) return;
    s.next_tick = now + 200;
    std::vector<Action> run; // game mode commands, run once the model lock is released
    try {
        const auto ui = read<Address>(base + build::engine::ui_manager);
        const auto manager = ui ? read<Address>(ui + 0x140) : 0;
        if (!manager) return;
        {
            const Context c(base, manager);
            game::ModelWriteLock lock(manager);
            ++s.pass;
            if (s.manager && s.manager != manager) forget(manager);
            s.manager = manager;
            s.base = base;
            if (s.page.handle && c.type_of(s.page.handle) != s.page.type) release(c);
            // `mode grid on|off` changed: build this page's cards again in the other layout.
            if (s.page.handle && !s.details && modes::throwdown_grid() != (s.grid.handle != 0)) {
                const std::pair<Handle, Handle> generation{s.page.handle, s.cards.handle};
                release(c);
                s.generations.erase(generation);
            }
            // Reopening Throwdowns makes a new page and list: follow them.
            for (const auto &p : c.roots({page.hash}))
                if (const auto found = card_list(c, p.model)) {
                    const auto list = found->list;
                    if (s.page.handle && (p.model.handle != s.page.handle || list.handle != s.cards.handle) &&
                        !s.generations.contains({p.model.handle, list.handle}))
                        release(c);
                    if (!s.page.handle && !s.generations.contains({p.model.handle, list.handle})) {
                        initialize(c, p.model, list, found->holder);
                        break;
                    }
                }
            std::vector<Action> pending;
            {
                std::lock_guard pending_lock(s.mutex);
                pending = std::exchange(s.pending, {});
            }
            if (!s.page.handle) {
                for (const auto &a : pending)
                    if (a.command == "mode") run.push_back(a);
            } else {
                // Keep the cards' callbacks reserved while a panel shows.
                for (const auto &card : s.mode_cards) callback(c, "card", card.key);
                shrink_all(c); // the native list can restore authored sizes when it remounts
                if (s.grid.handle) mount_grid(c);
                else publish_cards(c);
                for (const auto &a : pending) {
                    if (a.generation != s.owner.load()) continue;
                    // A mode's card sets that mode up (unless a game is on) and shows its panel.
                    if (a.command == "card") {
                        if (!modes::menu_view().in_game) run.push_back({a.generation, "mode", "new " + a.argument, a.pass});
                        switch_page(c, true);
                    }
                    else if (a.command == "back") switch_page(c, false);
                    else if (a.command == "mode") run.push_back(a);
                    // Placing flies the free camera over the world: close Throwdowns the way its own
                    // Back does (ThrowdownerExit), then start placing.
                    else if (a.command == "place") {
                        switch_page(c, false);
                        if (!navigate(c, "ThrowdownerExit")) s.status = "Close the menu to place.";
                        run.push_back({a.generation, "mode", "place " + a.argument, a.pass});
                    }
                }
                if (s.grid.handle) mount_grid(c); // straight back after Back restored the page
                if (s.details) render(c);
            }
        }
        for (const auto &a : run) {
            std::vector<std::string> words;
            std::size_t start = 0;
            while (start < a.argument.size()) {
                auto end = a.argument.find(' ', start);
                if (end == std::string::npos) end = a.argument.size();
                if (end > start) words.push_back(a.argument.substr(start, end - start));
                start = end + 1;
            }
            if (words.empty()) continue;
            const auto verb = words.front();
            words.erase(words.begin());
            s.status = modes::command(verb, words);
            logging::log(logging::Level::info, logging::Channel::ui, "Game modes card: mode {} -> {}", a.argument, s.status);
        }
    } catch (const std::exception &e) {
        logging::log(logging::Level::warning, logging::Channel::ui, "Game modes card: {}", e.what());
        s.next_tick = now + 5000;
        release_now(base, false);
    }
}
} // namespace dingosdk::multiplayer
