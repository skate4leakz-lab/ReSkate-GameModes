#include "one_up_menu.h"
#include "modes_card.h"
#include "native_menu_data.h"
#include "native_menu_lifetime.h"
#include "native_menu_ownership.h"
#include "native_hud_selection.h"
#include "native_hud_values.h"
#include "native_menu_dump.h"
#include "native_menu_internal.h"
#include "Extension/Throwdowns/one_up_runtime.h"
#include "Extension/Modes/game_modes.h"
#include "Extension/Modes/mode_rules.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Throwdowns/one_up_placement.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Customization/local_player_card_runtime.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_menu.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace dingosdk::multiplayer {
namespace {
constexpr bool own_card=false;
constexpr bool native_results_board=false;
using namespace menu_data;
namespace build = game::build::v20260929;
constexpr Schema tile_schema{0xdeb20e0f, 1200}, category_schema{0x628aa99c, 320},
    anchor_schema{0x0e3be640, 192}, label_schema{0xbfe23948, 104}, action_schema{0x85b7eac0, 72},
    panel_schema{0xd29bbf63,576}, header_schema{0x4efe5e19,312}, display_schema{0x1a647e6e,1040},
    panel_style_schema{0x1b6c904d,272}, setting_schema{0x94803e85,176}, selector_schema{0xf45cb4c0,1344},
    selector_item_schema{0x6d9d5106,312}, notice_schema{0x7290e514,256},
    texture_schema{0x370c9975,144}, content_tile_schema{0xcbe47f59,1184},
    page_action_schema{0x896f8a1a,784};
constexpr std::uint32_t content = 0x716496c8, items = 0x61742cb4, widget = 0x214d4984,
    rows_field = 0x67223da7, text_field = 0x4d8e01b9;
using Action = MenuAction;
struct Descriptor { alignas(8) std::array<std::byte, 0x70> bytes{}; Address info{}; Action action; };
struct Row { Value model, anchor, tile, selector; std::vector<Handle> choices; std::string text; Address callback{}; };
struct Original { Value tile, category, description; std::array<float,2> tile_size{}, description_size{}; float icon_width{}, icon_height{}; };
struct HudPlayer { Value row, background, name, stamps, strike_anchor, strike_item; std::array<Value,3> letters; bool out{}, stamps_row{}; };
struct ResultPlayer { Value row, rank, outcome; bool winner{}; };
struct State {
    // Borrowed native marker fields, restored when this match releases them.
    std::map<Handle,std::pair<Value,bool>> marker_originals;
    Address manager{}, base{}, anchor_asset{}, primary_input{};
    Value page, cards, card, category, description, details_list, original_body, original_back;
    Value source_label;
    Value panel_anchor, panel, header, badge, panel_style, panel_notice, footer_anchor, footer_list;
    Value footer_back, footer_confirm, duration_selector, confirm_action, original_actions;
    Value waiting_label, waiting_countdown, waiting_badge;
    Value hud_root, hud_stack, hud_notice, hud_anchor, hud_item, hud_timer, hud_timer_anchor, hud_timer_item;
    Value hud_content, hud_title, hud_target, hud_current, hud_clock, hud_clock_anchor, hud_clock_item, hud_board, hud_rows;
    Value hud_intro, hud_intro_badge, hud_intro_anchor, hud_intro_item;
    Value hud_results, hud_results_title, hud_results_notice, hud_results_rows, hud_results_anchor, hud_results_item;
    std::map<std::uint64_t,ResultPlayer> result_players;
    std::map<std::uint64_t,std::uint32_t> eliminated_at;
    std::uint64_t results_match{};
    std::uint32_t results_turn{};
    std::map<std::uint64_t,HudPlayer> hud_players;
    std::vector<std::uint64_t> hud_player_ids;
    bool hud_initialized{}, hud_countdown_visible{}, hud_intro_visible{}, hud_clock_shown{}, hud_results_visible{};
    int hud_kind=-1; // the feed the widgets were built for: 1 1-Up, 0 a game mode
    std::atomic<bool> hud_ready{};
    std::uint64_t hud_ended_at{}, hud_go_until{};
    std::uint64_t hud_countdown_token{}; // the countdown on screen (Feed::token)
    std::uint64_t waiting_diagnostic{};
    std::set<Handle> waiting_widgets;
    std::set<Handle> waiting_backups;
    std::map<Handle,Value> waiting_originals;
    std::map<Handle,std::pair<Value,Value>> waiting_presentations;
    std::map<Handle,Value> waiting_counts;
    std::map<Handle,std::pair<Value,Value>> official_originals;
    std::set<Handle> official_panels;
    std::map<Handle,Value> official_headers;
    std::map<Handle,Value> official_contents;
    std::map<Handle,Handle> official_slot_panels;
    std::map<Handle,Value> official_sliders;
    std::map<Handle,Value> solo_sliders;
    std::atomic<bool> reset_setup{};
    std::array<Value,1> body_items{};
    std::vector<Handle> duration_choices;
    std::map<std::string,Address,std::less<>> assets;
    OwnedMenuModels<Value> owned;
    std::vector<Original> originals;
    std::map<std::string, Row> rows;
    std::set<std::pair<Handle,Handle>> generations;
    std::vector<std::string> displayed;
    std::array<Descriptor, 128> descriptors;
    unsigned used{}, seconds=20, max_players=4;
    std::atomic<unsigned> entry_seconds{20}, entry_players{4};
    std::mutex mutex;
    std::vector<Action> pending;
    std::atomic<std::uint64_t> owner{};
    std::atomic<bool> setup_open{};
    std::uint64_t next_id{}, next_tick{}, pass{};
    std::string original_title;
    std::string setup_status;
    std::uint64_t place_until{};
    unsigned place_seconds{}, place_players{};
    bool details{};
};
State& state() { static auto* s = new State; return *s; }
std::string upper_text(std::string text);
void session_marker_controls(const Context& c,bool release=false);
Value reference(const Context& c, Ref ref) { return {ref.handle, c.type_of(ref.handle)}; }
Value primary(const Context& c, Value tile) {
    return c.path(tile, {widget, 0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
}
Value back(const Context& c, Value page_value) {
    return c.path(page_value, {0x10810f0b, 0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
}
Value body(const Context& c, Value p) { return c.field(c.element(c.path(p, {content, items}), 0), content); }
Value make(const Context& c, Schema schema, Address authored_type=0) {
    auto& s = state(); const auto id=(s.owner.load() ^ 0x3155500000000000ULL) + ++s.next_id;
    Value value;
    if(authored_type) {
        require((kind(authored_type)==2 || kind(authored_type)==4) && size(authored_type)==schema.size && read<std::uint32_t>(read<Address>(authored_type))==schema.hash,
                "1-Up authored model schema differs.");
        value={game::native_data().models.create(c.manager,authored_type,id,game::native_name_hash("ReSkate.NativeMultiplayerMenu"),false,2),authored_type};
        c.address(value);
    } else value=c.create(schema,id);
    s.owned.track(value); return value;
}
template<std::size_t I> void activate() noexcept {
    auto& s = state();
    try {
        std::unique_lock lock(s.mutex);
        if (I < s.used && s.descriptors[I].action.generation && s.descriptors[I].action.generation == s.owner.load() && s.pending.size() < 8 &&
            std::none_of(s.pending.begin(),s.pending.end(),[&](const auto& a){return a.command==s.descriptors[I].action.command && a.argument==s.descriptors[I].action.argument;})) {
            if(s.descriptors[I].action.command=="open") {
                // Reserve the original setup before any navigation is sent.
                // The flag bridge dispatches the verified native navigation
                // after old registrations have finished their cleanup.
                lock.unlock();
                if(!one_up::arm_native_setup(20,s.entry_players.load()))return;
                s.reset_setup.store(true);
                lock.lock();
            }
            s.pending.push_back(s.descriptors[I].action);
        }
    } catch (...) {}
}
template<std::size_t... I> auto functions(std::index_sequence<I...>) { return std::array{&activate<I>...}; }
Address callback(const Context& c, std::string command, std::string argument = {}) {
    auto& s = state(); std::lock_guard lock(s.mutex);
    const auto slot=action_slot(s.used,s.descriptors.size(),[&](std::size_t i)->const Action& {return s.descriptors[i].action;},command,argument,s.pass);
    require(slot.has_value(), "1-Up native action capacity reached.");
    static const auto f = functions(std::make_index_sequence<128>{});
    auto& d = s.descriptors[*slot];
    if(*slot==s.used) {
        const auto prototype = c.base + build::native_menu::action_prototype;
        require(read<Address>(prototype + 0x40) == c.base + build::native_menu::action_invoker && read<std::uint8_t>(prototype + 0x68) == 0, "1-Up action ABI differs.");
        require(memory::peek_bytes(prototype, d.bytes.data(), d.bytes.size()), "1-Up action prototype unavailable.");
        const auto invoker = reinterpret_cast<Address>(f[*slot]);
        for (const auto offset : {0x30,0x40,0x48}) std::memcpy(d.bytes.data()+offset, &invoker, 8);
        d.info = reinterpret_cast<Address>(d.bytes.data()); ++s.used;
    }
    d.action = {s.owner.load(),std::move(command),std::move(argument),s.pass};
    return reinterpret_cast<Address>(&d.info);
}
Address asset(const Context& c, const char* name) {
    auto& s=state();
    if(const auto cached=s.assets.find(name);cached!=s.assets.end())return cached->second;
    std::vector<Address> anchors;
    std::set<unsigned> domains;
    const auto add_anchor=[&](Address anchor) {
        if(anchor && domains.insert(read<std::uint16_t>(anchor+0x16)).second)anchors.push_back(anchor);
    };
    // CategoryTile_Widget is a shared foundation asset. Its parent domain
    // cannot find the Throwdown resources in the child domain. The retained
    // badge is owned by that same child bundle, even after its menu retires.
    const auto add_icon=[&](Value model,std::uint32_t field) {
        if(!model.handle || c.type_of(model.handle)!=model.type)return;
        const auto icon=read<Address>(c.address(c.field(model,field)));
        if(!icon)return;
        const auto type=read<Address>(icon+8);
        if(type==c.base+build::native_menu::texture_asset_type || type==c.base+build::native_menu::image_asset_type)
            add_anchor(icon);
    };
    add_icon(s.waiting_badge,0x6e469d2a);
    add_icon(s.category,0x189084da);
    add_anchor(s.anchor_asset);
    // Gameplay and menu presenters can own additional native widget domains.
    for(const auto& root:c.roots({presenter.hash})) {
        const auto blueprint=read<Widget>(root.value).blueprint;
        if(!blueprint || read<Address>(blueprint+8)!=c.base+build::native_menu::widget_blueprint_type)continue;
        add_anchor(blueprint);
        if(domains.size()>=32)break;
    }
    const auto find_loaded=[&](const char* path) {
        for(const auto anchor:anchors)if(const auto found=named_asset(c,anchor,path))return found;
        // Throwdown HUD resources are a child of the gameplay UI domain, so
        // walking a generic presenter's parents cannot reach them. Search
        // loaded child domains through the native lookup, bounded to the UI
        // domains observed on this build and linked to our current anchors.
        const auto find=game::native_data().find_asset;
        if(find)for(unsigned candidate=2;candidate<32;++candidate) {
            auto domain=static_cast<std::uint16_t>(candidate);
            std::set<std::uint16_t> visited;
            bool related=false;
            while(domain>1 && domain<0xbbf && visited.size()<32 && visited.insert(domain).second) {
                if(domains.contains(domain)) {related=true;break;}
                const auto owner=read<Address>(c.base+build::engine::domain_owners+domain*8ULL);
                if(!owner)break;
                domain=read<std::uint16_t>(owner+0x42);
            }
            if(related)if(const auto found=find(static_cast<std::uint16_t>(candidate),path))return found;
        }
        return Address{};
    };
    if(const auto found=find_loaded(name)) {s.assets.emplace(name,found);return found;}
    // Exported dynamic-model records are not indexed as ordinary assets.
    // Resolve their actual resource list, then select the exact exported name.
    constexpr std::array containers{
        std::pair{"Activities_DetailsMenu_ContentResource/","UI/Features/Activities/Resources/Activities_DetailsMenu_ContentResource"},
        std::pair{"Activities_DetailsMenu_ContentResource_Challenge/","UI/Features/Activities/Resources/Activities_DetailsMenu_ContentResource_Challenge"},
        std::pair{"StackPanelStylesList/","UI/Foundations/Components/StackVariations/StackPanel/StackPanelStylesList"},
        std::pair{"TD_HostParameters_ContentResource/","UI/Features/Throwdowns/ContentResources/TD_HostParameters_ContentResource"},
        std::pair{"TD_Page_ContentResources/","UI/Features/Throwdowns/TD_Page_ContentResources"},
        std::pair{"TD_Hud_ContentResources/","UI/Features/Throwdowns/TD_Hud_ContentResources"},
        std::pair{"TD_Scoreboard_ContentResources/","UI/Features/Throwdowns/TD_Scoreboard_ContentResources"},
        std::pair{"Activities_HudElementData_ContentResource/","UI/Features/Activities/Resources/Activities_HudElementData_ContentResource"},
        std::pair{"ActivityBannerStylesList/","UI/Features/Activities/Models/ActivityBannerStylesList"},
        std::pair{"SettingWrapperStylesList/","UI/Foundations/Components/Settings/Widgets/SettingWrapperStylesList"},
        std::pair{"ColorPaletteStylesList/","UI/Foundations/Styles/Lists/ColorPaletteStylesList"},
        std::pair{"TextStylesList/","UI/Foundations/Styles/Lists/TextStylesList"},
        std::pair{"NoticeStylesList/","UI/Foundations/Components/Notices/NoticeStylesList"},
        std::pair{"ButtonStyleList/","UI/Foundations/Styles/Lists/ButtonStyleList"},
        std::pair{"","UI/Features/Throwdowns/ContentResources/Throwdown_ModeSelect_ContentResources"}};
    for(const auto& [prefix,path]:containers) if(std::string_view(name).starts_with(prefix)) {
        const Address list=find_loaded(path);
        if(!list || read<Address>(list+8)!=c.base+build::native_menu::asset_list_type)continue;
        const auto records=read<Address>(list+0x20);
        const auto count=records?read<std::uint32_t>(records-4)&0x7fffffff:0;
        require(count<=2048,"1-Up native resource list exceeds its limit.");
        for(unsigned i=0;i<count;++i) {
            const auto record=read<Address>(records+i*8ULL);
            if(!record || read<Address>(record+8)!=c.base+build::native_menu::asset_record_type)continue;
            const auto exported_name=string(read<Address>(record+0x28),256);
            s.assets.emplace(exported_name,record);
        }
        if(const auto found=s.assets.find(name);found!=s.assets.end())return found->second;
    }
    // Shared foundation widgets can live outside the Throwdowns asset domain.
    // Reuse the native Multiplayer page's discovered, retained blueprints.
    for (unsigned slot=0; slot<menu_view::page_count; ++slot) {
        const auto& page_state = native_menu_detail::page_state(slot);
        if (page_state.base != c.base || page_state.manager != c.manager) continue;
        if (const auto found=page_state.assets.find(name); found!=page_state.assets.end() &&
            (read<Address>(found->second+8)==c.base+build::native_menu::widget_blueprint_type ||
             read<Address>(found->second+8)==c.base+build::native_menu::asset_record_type)) return found->second;
    }
    // Throwdowns can open before the separate ReSkate pause page has loaded.
    // Use the same bounded, validated foundation discovery directly instead
    // of depending on that unrelated page having populated its cache first.
    for(const auto& root:c.roots({core.hash})) {
        try {
            const auto found=blueprints(c,root.model);
            s.assets.insert(found.begin(),found.end());
            if(const auto loaded=s.assets.find(name);loaded!=s.assets.end())return loaded->second;
        } catch(const std::exception&) { }
    }
    throw std::runtime_error(std::string("1-Up foundation widget unavailable: ")+name);
}
Value category(const Context& c, Value tile) {
    const auto w = read<Widget>(c.address(c.path(tile, {widget, widget})));
    const auto model = reference(c,w.data); require(model.type && size(model.type)==category_schema.size && read<std::uint32_t>(read<Address>(model.type))==category_schema.hash, "1-Up category schema differs."); return model;
}
Value record_model(const Context& c, const char* name, Schema schema) {
    const auto record=asset(c,name);
    require(read<Address>(record+8)==c.base+build::native_menu::asset_record_type,"1-Up panel template is not a native record.");
    const auto type=read<Address>(record+0x18), data=read<Address>(record+0x20);
    require(type && data,"1-Up panel template data unavailable.");
    // A loaded authored record already carries validated native TypeInfo.
    // Its type need not have a live model yet. The native factory registers it.
    auto result=make(c,schema,type); c.copy(result,data); return result;
}
Value record_model(const Context& c,const char* name) {
    const auto record=asset(c,name), type=read<Address>(record+0x18);
    require(read<Address>(record+8)==c.base+build::native_menu::asset_record_type && type && kind(type)==2,
            "1-Up HUD template schema differs.");
    return record_model(c,name,{read<std::uint32_t>(read<Address>(type)),static_cast<std::uint16_t>(size(type))});
}
Value clone_widget_model(const Context& c,Value target) {
    auto w=read<Widget>(c.address(target));
    const auto type=w.data.handle?c.type_of(w.data.handle):(w.data.record?read<Address>(w.data.record+0x18):0);
    require(type && kind(type)==2,"Native HUD widget model unavailable.");
    const auto data=w.data.handle?c.address({w.data.handle,type}):read<Address>(w.data.record+0x20);
    const auto result=make(c,{read<std::uint32_t>(read<Address>(type)),static_cast<std::uint16_t>(size(type))},type);
    c.copy(result,data); w.data={0,result.handle}; c.set(target,w); return result;
}
// A typed native copy retains DataRefs; it does not clone the models they
// reference. Copy every mutable UI edge before mounting a private menu. Asset
// styles, expression contexts and backend bindings remain borrowed read-only.
struct PrivateUiGraph {
    const Context& c;
    UiCloneCache<std::pair<Handle,Address>,Value> copies;
    unsigned budget=2400;
    Value clone(Ref source,unsigned depth=0) {
        const auto type=source.handle?c.type_of(source.handle):(source.record?read<Address>(source.record+0x18):0);
        require(type && (kind(type)==2 || kind(type)==4),"Private Throwdown UI model is unavailable.");
        const auto key=std::pair{source.handle,source.handle?type:source.record};
        return copies.clone(key,[&] {
            require(depth<32 && budget,"Private Throwdown UI graph exceeds its limit.");--budget;
            const auto data=source.handle?c.address({source.handle,type}):read<Address>(source.record+0x20);
            const auto result=make(c,{read<std::uint32_t>(read<Address>(type)),static_cast<std::uint16_t>(size(type))},type);
            c.copy(result,data);return result;
        },[&](Value result){walk(result,depth+1);});
    }
    void walk(Value value,unsigned depth) {
        require(depth<32,"Private Throwdown UI graph depth differs.");
        const auto hash=read<std::uint32_t>(read<Address>(value.type));
        if(hash==presenter.hash) {
            auto w=read<Widget>(c.address(value));
            if(w.data.handle || w.data.record) {w.data={0,clone(w.data,depth).handle};c.set(value,w);}
            return;
        }
        if(hash==0x62088281) {
            const auto ref=read<Ref>(c.address(value));
            if(ref.handle || ref.record) {
                const auto type=ref.handle?c.type_of(ref.handle):read<Address>(ref.record+0x18);
                // Scalar backend expressions are borrowed read-only. Mutable
                // widgets/lists are private, including array-valued models.
                if(type && (kind(type)==2 || kind(type)==4))c.set(value,Ref{0,clone(ref,depth).handle});
            }
            return;
        }
        if(kind(value.type)==4) {
            unsigned count{},stride{};c.array(value,128,count,stride);
            for(unsigned i=0;i<count;++i) {
                const auto child=c.element(value,i);
                if(kind(child.type)==2 || kind(child.type)==4)walk(child,depth+1);
            }
        } else if(kind(value.type)==2) {
            const auto meta=read<Address>(value.type),fields=read<Address>(meta+0x60);
            const auto count=read<std::uint16_t>(meta+0x2a);
            require(count<=128,"Private Throwdown field count differs.");
            for(unsigned i=0;i<count;++i) {
                const auto key=read<std::uint32_t>(fields+i*24);
                if(key==0x8cc042ef || key==0xa1e84eb1 || key==0x25189231 || key==0xf94d8cc6 ||
                   key==0x8a3e2159 || key==0x54c9abee || key==0xd78d240b || key==0x85b8c3e6 || key==655725074U)continue;
                const auto child=c.field(value,key);
                if(key==0xf2c90867 && kind(child.type)==4) {
                    unsigned n{},stride{};const auto bytes=c.array(child,128,n,stride);
                    require(stride==sizeof(Handle),"Private UI handle array differs.");
                    std::vector<Handle> handles(n);if(n)std::memcpy(handles.data(),bytes.data(),bytes.size());
                    for(auto& handle:handles)if(handle)handle=clone({0,handle},depth+1).handle;
                    c.array(child,handles);
                } else if(kind(child.type)==2 || kind(child.type)==4)walk(child,depth+1);
            }
        }
    }
};
Value clone_widget_tree(const Context& c,Value slot) {
    auto w=read<Widget>(c.address(slot));
    PrivateUiGraph graph{c};const auto result=graph.clone(w.data);
    w.data={0,result.handle};c.set(slot,w);return result;
}
std::vector<Value> ui_models(const Context& c,Value root,std::uint32_t wanted) {
    std::vector<Value> result;std::set<Handle> seen;unsigned budget=2400;
    std::function<void(Value,unsigned)> visit=[&](Value value,unsigned depth) {
        if(!value.handle || !value.type || !budget || depth>24 || !seen.insert(value.handle).second)return;
        --budget;const auto hash=read<std::uint32_t>(read<Address>(value.type));
        if(hash==wanted) {result.push_back(value);return;}
        if(hash==0x62088281) {
            const auto ref=read<Ref>(c.address(value));if(ref.handle)visit(reference(c,ref),depth+1);return;
        }
        if(kind(value.type)==4) {
            unsigned count{},stride{};c.array(value,128,count,stride);
            for(unsigned i=0;i<count;++i)visit(c.element(value,i),depth+1);
        } else if(kind(value.type)==2) {
            const auto meta=read<Address>(value.type),fields=read<Address>(meta+0x60);
            const auto count=read<std::uint16_t>(meta+0x2a);require(count<=128,"Throwdown UI field count differs.");
            for(unsigned i=0;i<count;++i) {
                const auto key=read<std::uint32_t>(fields+i*24);
                if(key==0x8cc042ef || key==0xa1e84eb1 || key==0x25189231 || key==0xf94d8cc6 ||
                   key==0x8a3e2159 || key==0x54c9abee || key==0xd78d240b || key==0x85b8c3e6 || key==655725074U)continue;
                const auto child=c.field(value,key);
                if(key==0xf2c90867 && kind(child.type)==4) {
                    unsigned n{},stride{};const auto bytes=c.array(child,128,n,stride);require(stride==sizeof(Handle),"UI handle list differs.");
                    for(unsigned j=0;j<n;++j) {
                        Handle handle{};std::memcpy(&handle,bytes.data()+j*stride,stride);
                        if(handle)visit({handle,c.type_of(handle)},depth+1);
                    }
                } else if(kind(child.type)==2 || kind(child.type)==4)visit(child,depth+1);
            }
        }
    };
    visit(root,0);return result;
}
void hud_list(const Context& c,Value& list,const std::vector<Widget>& widgets,bool horizontal,float item_size,float spacing,float last_height=0.f,float width=850.f,float first_width=0.f,bool fit_first=false) {
    if(!list.handle)list=make(c,linear_list);
    // Stock StackComponent always binds a one-item focus window; its blueprint
    // does not connect MaxFocusedItemCount to the list element. A native linear
    // list presents every noninteractive anchored HUD item, without that owner.
    c.set(c.field(list,0x55ca89f4),asset(c,"UI/Foundations/Components/Lists/Shared/AnchoredContentPresenterListItem_Widget"));
    c.set(c.field(list,0x4cb61cac),horizontal?0:1);
    c.set(c.field(list,0x64b9a9e8),0); // Preserve the presenters' authored measures.
    c.set(c.field(list,0x19ff199a),item_size);
    c.set(c.field(list,0xebca7354),spacing);
    c.set(c.field(list,0x4554761c),false); // No clipping of the last, variable-height leaderboard.
    c.set(c.field(list,0xafe8434b),false); // No virtualization of penalty rows.
    c.set(c.field(list,0x8bbf7d67),true);
    c.array(c.field(list,rows_field),std::vector<Ref>{});
    std::vector<Handle> handles;
    for(unsigned i=0;i<widgets.size();++i) {
        const auto anchor=make(c,anchor_schema);
        c.set(c.field(anchor,widget),widgets[i]);
        c.set(c.field(anchor,0x144aee01),false);
        // The engine measures the name itself, so penalty stamps follow the
        // actual lettering rather than an empty fixed-width name column.
        c.set(c.field(anchor,0x6efc1a61),horizontal && i==0 && fit_first);
        const float height=(!horizontal && i+1==widgets.size() && last_height>0)?last_height:item_size;
        c.set(c.field(anchor,0x1cff7243),std::array<float,2>{horizontal?(i==0 && first_width>0?first_width:item_size):width,height});
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const auto layout=c.field(anchor,axis);
            c.set(c.field(layout,0x99bbed5a),0.f);c.set(c.field(layout,0x49943bed),0.f);
            c.set(c.field(layout,0xfec5e0b1),0.f);c.set(c.field(layout,0xd01f4a21),1.f);
        }
        handles.push_back(anchor.handle);
    }
    c.array(c.field(list,0xf2c90867),handles);
}
void native_timer_seconds(const Context& c,Value timer,std::uint32_t field,std::uint64_t milliseconds) {
    const auto value=c.field(timer,field);
    require(kind(value.type)==17,"Native countdown time is not Uint64.");
    const auto seconds=native_hud_seconds(milliseconds);
    // A new value starts the native number transition. Publish once per
    // displayed second rather than restarting its animation on every tick.
    if(read<std::uint64_t>(c.address(value))!=seconds)c.set(value,seconds);
}
bool hud_list_has_items(const Context& c,Value list,unsigned expected) {
    if(!list.handle)return false;
    unsigned count{},stride{};
    c.array(c.field(list,0xf2c90867),expected+1,count,stride);
    return count==expected;
}
Ref record_ref(const Context& c,const char* name) { return {asset(c,name),0}; }
void clear_value(const Context& c,Value value) {
    if(kind(value.type)==4) {
        // Native array publication reads the count preceding the pointer even
        // for an empty array. A null pointer is not an empty counted array.
        c.array(value,std::span<const std::byte>{},0); return;
    }
    if(kind(value.type)==2) {
        // Bindings can contain counted arrays and owned delegates. Reset from
        // a native-constructed value instead of fabricating zeroed storage.
        const auto defaults=make(c,{read<std::uint32_t>(read<Address>(value.type)),static_cast<std::uint16_t>(size(value.type))},value.type);
        c.copy(value,c.address(defaults)); return;
    }
    const std::vector<std::byte> empty(size(value.type)); c.publish(value,empty.data());
}
constexpr std::int32_t hud_score_id=0x31555071, hud_countdown_id=0x31555072, hud_intro_id=0x31555073;
constexpr std::int32_t hud_results_id=0x31555074;
constexpr std::int32_t hud_clock_id=0x31555075;
constexpr std::int32_t hud_strike_id=0x31555800;
void clear_hud_state() {
    auto& s=state();s.hud_ready.store(false);s.hud_ended_at=0;s.hud_go_until=0;s.hud_countdown_token={};
    s.hud_root={};s.hud_stack={};s.hud_notice={};s.hud_anchor={};s.hud_item={};
    s.hud_timer={};s.hud_timer_anchor={};s.hud_timer_item={};
    s.hud_intro={};s.hud_intro_badge={};s.hud_intro_anchor={};s.hud_intro_item={};
    s.hud_content={};s.hud_title={};s.hud_target={};s.hud_current={};s.hud_clock={};s.hud_clock_anchor={};s.hud_clock_item={};
    s.hud_board={};s.hud_rows={};s.hud_players.clear();s.hud_player_ids.clear();
    s.hud_results={};s.hud_results_title={};s.hud_results_notice={};s.hud_results_rows={};
    s.hud_results_anchor={};s.hud_results_item={};s.result_players.clear();s.eliminated_at.clear();s.results_match=0;s.results_turn=0;
    s.hud_initialized=false;s.hud_countdown_visible=false;s.hud_intro_visible=false;s.hud_clock_shown=false;s.hud_results_visible=false;s.hud_kind=-1;
}
void publish_hud_items(const Context& c,bool score,bool countdown,bool intro=false,bool results=false) {
    auto& s=state();if(!s.hud_stack.handle)return;
    if(c.type_of(s.hud_stack.handle)!=s.hud_stack.type) {s.hud_ready.store(false);return;}
    const auto target=c.field(s.hud_stack,items);
    unsigned count{},stride{};const auto old=c.array(target,32,count,stride);
    require(stride==stack_item.size,"Native HUD stack schema differs.");
    const auto offset=c.member(c.type(stack_item),0x1e95752c).offset;
    std::vector<std::byte> next;
    for(unsigned i=0;i<count;++i) {
        std::int32_t id{};std::memcpy(&id,old.data()+i*stride+offset,4);
        if(id!=hud_score_id && id!=hud_countdown_id && id!=hud_intro_id && id!=hud_results_id && id!=hud_clock_id && !(id>=hud_strike_id && id<hud_strike_id+6))next.insert(next.end(),old.begin()+i*stride,old.begin()+(i+1)*stride);
    }
    for(const auto item:{score?s.hud_item:Value{},score && s.hud_clock_shown?s.hud_clock_item:Value{},countdown?s.hud_timer_item:Value{},intro?s.hud_intro_item:Value{},results?s.hud_results_item:Value{}})if(item.handle) {
        const auto start=next.size();next.resize(start+stride);
        require(memory::peek_bytes(c.address(item),next.data()+start,stride),"Native HUD item unavailable.");
    }
    if(score)for(const auto id:s.hud_player_ids)if(const auto& player=s.hud_players.at(id);player.out && player.strike_item.handle) {
        const auto start=next.size();next.resize(start+stride);
        require(memory::peek_bytes(c.address(player.strike_item),next.data()+start,stride),"Native elimination stroke unavailable.");
    }
    if(next!=old)c.array(target,next,static_cast<unsigned>(next.size()/stride));
}
void plain_label(const Context& c,Value value,const std::string& text) {
    if(c.text(c.field(value,text_field),2048)!=text) c.text(c.field(value,text_field),text);
    c.set(c.field(value,0x042924a4),true);
}
void restore_official(const Context& c) {
    auto& s=state();
    std::set<Handle> owned_panels;
    for(const auto& [panel,header]:s.official_headers) {
        if(c.type_of(panel)!=c.type(panel_schema) || !game::native_data().models.value(c.manager,panel,0,1))continue;
        if(read<Widget>(c.address(c.field({panel,c.type(panel_schema)},0xc0f1b449))).data.handle==header.handle)owned_panels.insert(panel);
    }
    for(const auto& [id,pair]:s.official_originals) {
        // A retired panel can still have a typed handle after its embedded
        // storage disappears. That must not block every later menu update.
        if(c.type_of(id)!=pair.first.type || !game::native_data().models.value(c.manager,id,0,1))continue;
        // Body is an inline native StackComponent, whereas header/footer are
        // Widget presenters. Use the private header to identify panel ownership
        // before restoring any of the three, including their inline arrays.
        const auto panel=s.official_slot_panels.find(id);
        if(panel!=s.official_slot_panels.end() && owned_panels.contains(panel->second))
            c.copy(pair.first,c.address(pair.second));
    }
    s.official_originals.clear(); s.official_panels.clear(); s.official_headers.clear(); s.official_contents.clear(); s.official_slot_panels.clear(); s.official_sliders.clear();
}
void remember_official(const Context& c,Value value) {
    auto& originals=state().official_originals;
    if(originals.contains(value.handle))return;
    const auto copy=make(c,{read<std::uint32_t>(read<Address>(value.type)),static_cast<std::uint16_t>(size(value.type))},value.type);
    c.copy(copy,c.address(value)); originals.emplace(value.handle,std::pair{value,copy});
}
void label_styles(const Context& c,Value label,const char* idle,const char* focused) {
    c.set(c.field(label,0xf94d8cc6),record_ref(c,idle));
    c.set(c.field(label,0xa1e84eb1),record_ref(c,focused));
    c.set(c.field(label,0xfc23a999),record_ref(c,"TextStylesList/B52-SemiB_Black"));
    c.set(c.field(label,0x25189231),record_ref(c,focused));
}
Value compact_anchor(const Context& c,Value model,const char* blueprint,float height,bool focusable) {
    auto result=make(c,anchor_schema);
    c.set(c.field(result,widget),Widget{asset(c,blueprint),{0,model.handle}});
    c.set(c.field(result,0x1cff7243),std::array<float,2>{1130.f,height});
    c.set(c.field(result,0x6efc1a61),false); c.set(c.field(result,0xbe5683d9),false);
    c.set(c.field(result,0x144aee01),focusable); return result;
}
void compact_panel(const Context& c) {
    auto& s=state(); if(s.panel.handle)return;
    s.panel=record_model(c,"Activities_DetailsMenu_ContentResource/Activities_Details_StackPanel",panel_schema);
    s.panel_anchor=record_model(c,"Activities_DetailsMenu_ContentResource/Activities_Details_AnchoredContentPresenter",anchor_schema);
    // This is the original compact Throwdown panel. All mutable content is
    // owned by 1-Up; the three native Throwdown models remain untouched.
    c.set(c.field(s.panel_anchor,widget),Widget{asset(c,"UI/Foundations/Components/StackVariations/StackPanel/StackPanel_Widget"),{0,s.panel.handle}});
    clear_value(c,c.field(s.panel_anchor,0x1aa5017f));
    // The authored Throwdown creation style supplies the same panel grain,
    // margins and footer dimensions as S.K.A.T.E.'s setup. Center it in the
    // page instead of inheriting the activity browser's left-column anchor.
    // RimeAxisLayoutData is stored as offsets/start/pivot/end/weight, not
    // in the order used by its authored schema. Publish named fields so the
    // panel cannot accidentally anchor at zero and extend off the left edge.
    for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
        const auto layout=c.field(s.panel_anchor,axis);
        const bool vertical=axis==0x185a5736U;
        c.set(c.field(layout,0x99bbed5a),vertical?0.f:.5f); // AnchorStart
        c.set(c.field(layout,0x49943bed),vertical?1.f:.5f); // AnchorEnd
        c.set(c.field(layout,0x40bfbff4),vertical?100.f:0.f); // OffsetStart
        c.set(c.field(layout,0x1d233266),vertical?-100.f:0.f); // OffsetEnd
        c.set(c.field(layout,0xfec5e0b1),.5f); // SizingPivot
        c.set(c.field(layout,0xd01f4a21),vertical?0.f:1.f); // SizingWeight
    }
    c.set(c.field(s.panel_anchor,0x6efc1a61),true);
    c.set(c.field(s.panel_anchor,0xbe5683d9),false);
    s.panel_style=record_model(c,"StackPanelStylesList/StackPanelStyle.ThrowdownCreation",panel_style_schema);
    c.set(c.field(s.panel_style,0xc13ba637),false);
    c.set(c.field(s.panel,0x8cc042ef),Ref{0,s.panel_style.handle});
    s.header=record_model(c,"Activities_DetailsMenu_ContentResource/Activities_Details_StackPanel_HeaderContent",header_schema);
    plain_label(c,c.field(s.header,0x44688629),"Throwdown");
    label_styles(c,c.field(s.header,0x44688629),"TextStylesList/B52-SemiB_Bright","TextStylesList/B52-SemiB_Bright");
    plain_label(c,c.field(s.header,0x00f3b15e),"1-UP");
    label_styles(c,c.field(s.header,0x00f3b15e),"TextStylesList/D180-Caps_White","TextStylesList/H72-XBold_White");
    c.set(c.path(s.header,{0x00f3b15e,0xfc53d427}),false);
    c.set(c.field(s.header,0x2840ecf6),Widget{});
    // FramedIcon paints a solid plate; its palette is RGB, not RGBA. The native
    // Texture widget preserves the badge's encoded alpha without a frame.
    s.badge=make(c,texture_schema);
    c.copy(c.field(s.badge,0x6e469d2a),c.address(c.field(s.originals[2].category,0x189084da)));
    c.set(c.field(s.badge,0xf524c836),288.f);
    c.set(c.field(s.badge,0x6fbd254e),288.f);
    c.set(c.field(s.badge,0x81c94d8a),false);
    c.set(c.field(s.badge,0xd1997d0c),false);
    c.set(c.field(s.badge,0x190e039c),false);
    c.set(c.field(s.header,content),Widget{asset(c,"UI/Foundations/Components/Media/Icons/Texture_Widget"),{0,s.badge.handle}});
    c.set(c.field(s.panel,0xc0f1b449),Widget{asset(c,"UI/Foundations/Components/ContentPresenterVariations/ContentWithTitle_Widget"),{0,s.header.handle}});
    s.panel_notice=record_model(c,"TD_Page_ContentResources/TD_Notice_MenuDescriptionTitle",notice_schema);
    plain_label(c,c.field(s.panel_notice,0x94703efc),"Beat the previous turn total. Three penalties and you're out.\nLast skater wins!");
    c.set(c.field(s.panel_notice,0x13be0d51),std::array<float,2>{1120.f,200.f});
    label_styles(c,c.field(s.panel_notice,0x94703efc),"TextStylesList/B52-SemiB_Bright","TextStylesList/B52-SemiB_Bright");
    c.set(c.field(s.panel,0x4fd7853d),Widget{});
    // Rules belong in the body, under the title. HeaderSecondaryContent is
    // an overlay intended for the mode notice, not the rules description.
    auto& rules=s.rows["rules"];
    rules.model=s.panel_notice;
    rules.anchor=compact_anchor(c,s.panel_notice,"UI/Foundations/Components/Notices/BasicNotice_Widget",200.f,false);
    auto first=make(c,stack_item);
    s.body_items={first};
    for(unsigned index=0;index<s.body_items.size();++index) {
        const auto item=s.body_items[index];
        c.set(c.field(item,0x1e95752c),static_cast<int>(0x31555010U+index));
        // StackComponentItem resolves its layout through Transition. Native
        // pushes supply this reference; directly published items must too.
        // Progress/IsActive alone do not give the widget a visible layout.
        c.copy(c.field(item,0xd78d240b),c.address(c.path(s.panel,{0x43967ece,0x54c9abee})));
    }
    c.set(c.field(first,content),Widget{asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),{0,s.details_list.handle}});
    c.set(c.field(first,0x369babfe),true);
    // Fresh StackItems begin at interpolation progress zero, so their content
    // stays collapsed until the stack controller receives a show transition.
    // The panel body is permanently visible; initialize both tracks as shown.
    for(const auto item:s.body_items)for(const auto track:{0x8c0583eeU,0xc5c7cc11U}) {
        c.set(c.path(item,{track,0x20247753}),1.f);
        c.set(c.path(item,{track,0x2426103f}),1.f);
        c.set(c.path(item,{track,0x1e3ce013}),true);
    }
    std::vector<std::byte> item_bytes(stack_item.size);
    require(memory::peek_bytes(c.address(first),item_bytes.data(),stack_item.size),"1-Up panel items unavailable.");
    c.array(c.path(s.panel,{0x43967ece,items}),item_bytes,1);
    c.set(c.path(s.panel,{0x43967ece,0x1fbbb5da}),true);
    c.set(c.path(s.panel,{0x43967ece,0x369babfe}),true);
    c.set(c.path(s.panel,{0x43967ece,0xc912aa1d}),false);
    c.set(c.path(s.panel,{0x43967ece,0x2a98baa3}),false);
    c.set(c.path(s.panel,{0x43967ece,0x18f8355b}),-1);
    s.footer_anchor=record_model(c,"Activities_DetailsMenu_ContentResource/Activities_Details_StackPanel_Footer_AnchoredContentPresenter",anchor_schema);
    s.footer_list=record_model(c,"Activities_DetailsMenu_ContentResource/Activities_Details_StackPanel_FooterContent",linear_list);
    s.footer_back=record_model(c,"TD_Page_ContentResources/TD_Back_Button",button);
    s.footer_confirm=record_model(c,"TD_Page_ContentResources/TD_Create_Button",button);
    for(const auto b:{s.footer_back,s.footer_confirm}) {
        c.copy(c.field(b,0x58c9d354),c.address(s.source_label));
        label_styles(c,c.field(b,0x58c9d354),"TextStylesList/B52-SemiB-Caps_Dark","TextStylesList/B52-SemiB_Black");
        c.set(c.path(b,{0x58c9d354,0x3d8639d0}),2);
        c.set(c.field(b,0x659db23e),false);
        // These predicates are tied to the native mode's backend queue.
        // 1-Up uses its own coordinator and supplies its own availability.
        clear_value(c,c.field(b,0x67212c85));
    }
    c.set(c.path(s.footer_back,{0x0fb0d794,0x8cc042ef}),record_ref(c,"ButtonStyleList/ButtonStyle.CTA.Default"));
    c.set(c.path(s.footer_confirm,{0x0fb0d794,0x8cc042ef}),record_ref(c,"ButtonStyleList/ButtonStyle.CTA.Hero"));
    c.array(c.field(s.footer_list,rows_field),std::vector<Ref>{{0,s.footer_back.handle},{0,s.footer_confirm.handle}});
    c.set(c.field(s.footer_anchor,widget),Widget{asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),{0,s.footer_list.handle}});
    c.set(c.field(s.panel,0xbfc64535),Widget{asset(c,"UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget"),{0,s.footer_anchor.handle}});
    const auto action_type=read<Address>(read<Address>(c.field(s.page,0xd2118ca8).type)+0x30);
    s.confirm_action=make(c,page_action_schema,action_type);
    c.copy(c.field(s.confirm_action,0xa704272a),c.address(c.path(s.footer_confirm,{0x0fb0d794,0xa704272a})));
    s.primary_input=0;
    const auto native_actions=c.field(s.original_actions,0xd2118ca8);
    unsigned action_count{},action_stride{};
    c.array(native_actions,8,action_count,action_stride);
    require(action_stride==page_action_schema.size,"1-Up native page action schema differs.");
    for(unsigned i=0;i<action_count && !s.primary_input;++i) {
        const auto original=c.path(c.element(native_actions,i),{0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
        const auto candidate=read<Address>(c.address(c.field(original,0x3d00de27)));
        if(candidate && string(read<Address>(candidate+0x18))=="Configurations/Input/UI/UI_PrimaryAction") s.primary_input=candidate;
    }
    if(!s.primary_input) s.primary_input=asset(c,"Configurations/Input/UI/UI_PrimaryAction");
    const auto confirm=c.path(s.confirm_action,{0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
    c.set(c.field(confirm,0x3d00de27),s.primary_input);
    c.set(c.field(confirm,0x0169c2db),s.primary_input);
    logging::write(logging::Level::info,logging::Channel::ui,"1-Up: authored Throwdown creation layout, settings and Confirm controls constructed.");
}
Value description(const Context& c, Value cat) { return reference(c, read<Widget>(c.address(c.field(cat,widget))).data); }
std::optional<Value> card_list(const Context& c, Value p) {
    try {
        auto w = read<Widget>(c.address(body(c,p)));
        for (unsigned depth=0; depth<4; ++depth) {
            auto value = reference(c,w.data); if (!value.type) return {};
            const auto hash = read<std::uint32_t>(read<Address>(value.type));
            if (hash == linear_list.hash && size(value.type)==linear_list.size) {
                unsigned count{}, stride{}; auto bytes=c.array(c.field(value,rows_field),4,count,stride);
                if (count!=3 || stride!=sizeof(Ref)) return {};
                // The live Skate Jam card navigates through ThrowdownerActive;
                // JamSession is its gameplay mode, not its menu action name.
                const std::array<std::string_view,3> expected{"ThrowdownSkate","SpotBattle","ThrowdownerActive"};
                for (unsigned i=0;i<3;++i) {
                    Ref ref{}; std::memcpy(&ref,bytes.data()+i*stride,stride); auto t=reference(c,ref);
                    if (!t.type || size(t.type)!=tile_schema.size || read<std::uint32_t>(read<Address>(t.type))!=tile_schema.hash ||
                        c.text(c.field(primary(c,t),0xb1e14cd6)) != expected[i]) return {};
                }
                return value;
            }
            if (hash!=anchor_schema.hash || size(value.type)!=anchor_schema.size) return {};
            w=read<Widget>(c.address(c.field(value,widget)));
        }
    } catch (...) {}
    return {};
}
void switch_page(const Context& c, bool details) {
    auto& s=state();
    if (details) {
        compact_panel(c);
        c.set(body(c,s.page),Widget{asset(c,"UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget"),{0,s.panel_anchor.handle}});
        c.set(c.field(back(c,s.page),0x9c76b86c),callback(c,"back"));
        c.text(c.field(back(c,s.page),0xb1e14cd6),"");
        c.text(c.path(s.page,{0x3781b603,text_field}),"");
        request_ui_dump("0xd29bbf63",2);
    } else {
        c.copy(body(c,s.page),c.address(s.original_body));
        c.copy(back(c,s.page),c.address(s.original_back));
        c.copy(c.field(s.page,0xd2118ca8),c.address(c.field(s.original_actions,0xd2118ca8)));
        c.text(c.path(s.page,{0x3781b603,text_field}),s.original_title);
    }
    s.details=details;
    s.setup_open.store(details,std::memory_order_release);
}
void release(const Context& c) {
    auto& s=state(); s.owner.store(0);
    session_marker_controls(c,true);
    s.solo_sliders.clear();
    for(const auto& [handle,presentation]:s.waiting_presentations)
        if(c.type_of(handle)==presentation.first.type) {
            const auto current=read<Widget>(c.address(presentation.first)).data.handle;
            const bool owned=std::any_of(s.waiting_originals.begin(),s.waiting_originals.end(),[&](const auto& entry) {
                return current==entry.second.handle || current==c.field(entry.second,0xc13bcfcd).handle;
            });
            if(owned)c.copy(presentation.first,c.address(presentation.second));
        }
    s.waiting_presentations.clear();
    { std::lock_guard lock(s.mutex); s.pending.clear(); }
    s.owned.release([&] {
        publish_hud_items(c,false,false);s.hud_ready.store(false);
        restore_official(c);
    s.waiting_originals.clear(); s.waiting_widgets.clear(); s.waiting_backups.clear(); s.waiting_presentations.clear(); s.waiting_counts.clear(); s.waiting_label={}; s.waiting_countdown={}; s.waiting_badge={}; s.waiting_diagnostic=0;
        if (s.page.handle && c.type_of(s.page.handle)==s.page.type && s.details) switch_page(c,false);
        if (s.cards.handle && c.type_of(s.cards.handle)==s.cards.type) {
            unsigned count{},stride{}; auto bytes=c.array(c.field(s.cards,rows_field),8,count,stride);
            std::vector<Ref> retained;
            require(stride==sizeof(Ref),"1-Up card list changed during cleanup.");
            for (unsigned i=0;i<count;++i) { Ref ref{}; std::memcpy(&ref,bytes.data()+i*stride,stride); if(ref.handle!=s.card.handle) retained.push_back(ref); }
            c.array(c.field(s.cards,rows_field),retained);
        }
        for (const auto& o:s.originals) {
            if (c.type_of(o.tile.handle)==o.tile.type) c.set(c.field(o.tile,0x868043e1),o.tile_size);
            if (c.type_of(o.category.handle)==o.category.type) { c.set(c.field(o.category,0x495ffd43),o.icon_width); c.set(c.field(o.category,0x24fb5ca8),o.icon_height); }
            if (c.type_of(o.description.handle)==o.description.type) c.set(c.field(o.description,0x1cff7243),o.description_size);
        }
    },[&](Value value){c.destroy(value);});
    s.page={}; s.cards={}; s.card={}; s.panel={}; s.panel_anchor={}; s.badge={}; s.duration_selector={}; s.body_items={}; s.confirm_action={}; s.original_actions={};
    s.rows.clear(); s.originals.clear(); s.assets.clear(); s.displayed.clear(); s.duration_choices.clear(); s.details=false;
    clear_hud_state();
    s.setup_open.store(false,std::memory_order_release);
}
void initialize(const Context& c, Value p, Value list) {
    auto& s=state(); s.page=p; s.cards=list; s.owner.store(p.handle); s.next_id=0;
    unsigned count{},stride{}; auto bytes=c.array(c.field(list,rows_field),3,count,stride);
    std::vector<Ref> refs(count); std::memcpy(refs.data(),bytes.data(),bytes.size());
    for (const auto& ref:refs) {
        auto tile=reference(c,ref), cat=category(c,tile), desc=description(c,cat);
        s.originals.push_back({tile,cat,desc,read<std::array<float,2>>(c.address(c.field(tile,0x868043e1))),read<std::array<float,2>>(c.address(c.field(desc,0x1cff7243))),
            read<float>(c.address(c.field(cat,0x495ffd43))),read<float>(c.address(c.field(cat,0x24fb5ca8)))});
    }
    const auto donor=s.originals[1]; const auto card_widget=read<Widget>(c.address(c.path(donor.tile,{widget,widget})));
    s.anchor_asset=card_widget.blueprint;
    s.source_label=reference(c,read<Widget>(c.address(c.field(donor.description,widget))).data);
    require(s.source_label.type && size(s.source_label.type)==label_schema.size,"1-Up description label unavailable.");
    s.card=make(c,tile_schema); c.copy(s.card,c.address(donor.tile));
    s.category=make(c,category_schema); c.copy(s.category,c.address(donor.category));
    for (const auto hash : {0x189084daU, 0xc2917efcU})
        c.copy(c.field(s.category, hash), c.address(c.field(s.originals[2].category, hash)));
    s.description=make(c,anchor_schema); c.copy(s.description,c.address(donor.description));
    auto label=make(c,label_schema); c.copy(label,c.address(s.source_label));
    c.text(c.field(label,text_field),"Take turns beating the last total. Three penalties and you're out."); c.set(c.field(label,0x042924a4),true);
    auto desc_widget=read<Widget>(c.address(c.field(s.description,widget))); desc_widget.data={0,label.handle}; c.set(c.field(s.description,widget),desc_widget);
    auto cat_widget=read<Widget>(c.address(c.field(s.category,widget))); cat_widget.data={0,s.description.handle}; c.set(c.field(s.category,widget),cat_widget);
    c.text(c.path(s.category,{0x77b8ed0b,text_field}),"1-UP"); c.set(c.path(s.category,{0x77b8ed0b,0x042924a4}),true);
    auto cw=card_widget; cw.data={0,s.category.handle}; c.set(c.path(s.card,{widget,widget}),cw);
    c.text(c.field(primary(c,s.card),0xb1e14cd6),""); c.set(c.field(primary(c,s.card),0x9c76b86c),callback(c,"open"));
    // Four native cards fit the same authored row. Resize only live view models.
    const auto resize=[&](Value t,Value cat,Value desc,const Original& o) {
        auto dimensions=o.tile_size; dimensions[0]*=.74f; c.set(c.field(t,0x868043e1),dimensions);
        auto ds=o.description_size; ds[0]*=.74f; c.set(c.field(desc,0x1cff7243),ds);
        c.set(c.field(cat,0x495ffd43),o.icon_width*.74f); c.set(c.field(cat,0x24fb5ca8),o.icon_height*.74f);
    };
    for(const auto& o:s.originals) resize(o.tile,o.category,o.description,o);
    resize(s.card,s.category,s.description,donor);
    s.original_body=make(c,presenter); c.copy(s.original_body,c.address(body(c,p)));
    s.original_back=make(c,action_schema); c.copy(s.original_back,c.address(back(c,p)));
    s.original_title=c.text(c.path(p,{0x3781b603,text_field}));
    s.original_actions=make(c,page);
    c.copy(c.field(s.original_actions,0xd2118ca8),c.address(c.field(p,0xd2118ca8)));
    // The card only needs its three live siblings. Setup content is adapted
    // after the native screen opens; unavailable optional widgets must never
    // prevent the fourth card from appearing.
    refs.push_back({0,s.card.handle}); c.array(c.field(list,rows_field),refs);
    s.generations.emplace(p.handle,list.handle);
    logging::write(logging::Level::info,logging::Channel::ui,"1-Up: fourth native Throwdowns card registered.");
}
void row(const Context& c,std::vector<std::string>& shown,const std::string& id,const std::string& text,std::string command={},std::string argument={}) {
    auto& s=state(); auto [entry,fresh]=s.rows.try_emplace(id); auto& r=entry->second;
    const bool button_row=!command.empty();
    if(fresh) {
        r.model=make(c,button_row ? button : label_schema); r.anchor=make(c,anchor_schema);
        auto l=button_row ? c.field(r.model,0x58c9d354) : r.model; c.copy(l,c.address(s.source_label)); c.set(c.field(l,0x042924a4),true);
        c.set(c.field(r.anchor,widget),Widget{asset(c,button_row?"UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget":"UI/Foundations/Components/Text/Label/Widget/Label_Widget"),{0,r.model.handle}});
        c.set(c.field(r.anchor,0x1cff7243),std::array<float,2>{1130.f,button_row?144.f:100.f});
        c.set(c.field(r.anchor,0x6efc1a61),false); c.set(c.field(r.anchor,0xbe5683d9),false); c.set(c.field(r.anchor,0x144aee01),button_row);
    }
    auto l=button_row?c.field(r.model,0x58c9d354):r.model;
    if(c.text(c.field(l,text_field),2048)!=text) c.text(c.field(l,text_field),text);
    r.text=text;
    // Constructors can restore button defaults after mounting. Reconcile the
    // native styles so focused buttons do not become white text on white fill.
    c.set(c.field(l,0x042924a4),true);
    c.set(c.field(l,0xfc53d427),false);
    for(const auto hash:{0xf94d8cc6U,0xa1e84eb1U,0xfc23a999U,0x25189231U}) {
        const auto source=button_row?hash:0xf94d8cc6U;
        const auto wanted=read<Ref>(c.address(c.field(s.source_label,source)));
        const auto current=read<Ref>(c.address(c.field(l,hash)));
        if(current.record!=wanted.record || current.handle!=wanted.handle) c.set(c.field(l,hash),wanted);
    }
    if(button_row) {
        c.set(c.field(r.model,0x659db23e),false);
        c.set(c.field(l,0x3d8639d0),0);
        const auto& native_page=native_menu_detail::page_state(0);
        if(native_page.manager==c.manager && native_page.menu_style.handle && c.type_of(native_page.menu_style.handle)==native_page.menu_style.type)
            c.set(c.path(r.model,{0x0fb0d794,0x8cc042ef}),Ref{0,native_page.menu_style.handle});
    } else {
        c.set(c.field(l,0xcd71a279),false);
        c.set(c.field(l,0x3fa0b887),false);
    }
    if(button_row) {
        const auto at=c.path(r.model,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
        const auto action=callback(c,std::move(command),std::move(argument));
        if(r.callback!=action) { c.set(c.field(at,0x9c76b86c),action); c.text(c.field(at,0xb1e14cd6),""); r.callback=action; }
    }
    shown.push_back(id);
}
void setting_row(const Context& c,std::vector<std::string>& shown,const std::string& id,const std::string& title,const std::string& value,bool selectable=false,bool embedded=false) {
    auto& s=state(); auto [entry,fresh]=s.rows.try_emplace(id); auto& r=entry->second;
    const bool players=id=="players-setting";
    if(fresh) {
        r.model=make(c,setting_schema);
        auto label=c.field(r.model,0x58c9d354); c.copy(label,c.address(s.source_label));
        label_styles(c,label,"TextStylesList/H64-XBold_White","TextStylesList/H64-XBold_Black");
        c.set(c.field(r.model,0xc9bf367d),record_ref(c,"SettingWrapperStylesList/Horizontal.WithDivider"));
        c.set(c.field(r.model,0xf3d74cdd),0);
        Value control;
        if(selectable) {
            control=record_model(c,"TD_HostParameters_ContentResource/TD_Timer_BasicSelector",selector_schema);
            // The original selector's two-way bindings belong to Skate Jam.
            // Own the choices and observe only this selector's selected index.
            for(const auto field:{0x4256fd65U,0x85b8c3e6U,0xe88c0f54U}) clear_value(c,c.field(control,field));
            c.set(c.field(control,0xeb92bf1c),Ref{});
            for(const auto field:{0x41b10c27U,0xd637e1f4U}) c.set(c.field(control,field),Address{});
            auto list=c.field(control,0x760fc12b); c.set(c.field(list,0x340cfb95),Address{});
            r.choices.clear();
            for(unsigned n=players?1U:10U;n<=(players?6U:120U);n+=players?1U:10U) {
                const auto option=std::to_string(n)+(players?"":" sec");
                auto choice=make(c,selector_item_schema), text=make(c,label_schema);
                c.copy(text,c.address(s.source_label));
                label_styles(c,text,"TextStylesList/H64-XBold_White","TextStylesList/H64-XBold_Black");
                plain_label(c,text,option);
                c.text(c.field(choice,0x58c9d354),option);
                c.set(c.field(choice,0x34c8bcc2),Ref{0,text.handle}); r.choices.push_back(choice.handle);
            }
            c.array(c.field(list,rows_field),std::vector<Ref>{});
            c.array(c.field(list,0xf2c90867),r.choices);
            c.set(c.field(list,0x55511280),static_cast<int>(players?s.max_players-1:s.seconds/10-1));
            c.set(c.field(list,0x783c4b49),r.choices[players?s.max_players-1:s.seconds/10-1]);
            c.set(c.field(control,0x369babfe),true);
            c.set(c.field(r.model,content),Widget{asset(c,"UI/Foundations/Components/Settings/Widgets/BasicSelector_Widget"),{0,control.handle}});
            r.selector=control; r.callback=callback(c,players?"players":"duration");
        } else {
            control=make(c,label_schema); c.copy(control,c.address(s.source_label));
            label_styles(c,control,"TextStylesList/H64-XBold_White","TextStylesList/H64-XBold_Black");
            plain_label(c,control,value); c.set(c.field(control,0x3d8639d0),2);
            c.set(c.field(r.model,content),Widget{asset(c,"UI/Foundations/Components/Text/Label/Widget/Label_Widget"),{0,control.handle}});
        }
        if(selectable && !embedded) {
            // The original Throwdown list wraps settings in ContentPresenterTile;
            // SettingWrapper alone changes label colors but has no blue fill.
            r.tile=make(c,content_tile_schema);
            c.set(c.field(r.tile,widget),Widget{asset(c,"UI/Foundations/Components/Settings/Widgets/SettingWrapper_Widget"),{0,r.model.handle}});
            c.set(c.field(r.tile,0xa04998ca),false);
            c.set(c.field(r.tile,0xbc750e6a),true);
            c.set(c.path(r.tile,{0x0fb0d794,0x8cc042ef}),record_ref(c,"ButtonStyleList/TileButton.RoughPartial.Default"));
            c.set(c.path(r.tile,{0x0fb0d794,0xa704272a,0xc52416ef,0x0abf7c31}),Ref{0,r.model.handle});
            r.anchor=compact_anchor(c,r.tile,"UI/Foundations/Components/Buttons/ContentPresenterTile/ContentPresenterTile",172.f,true);
        } else if(!embedded) r.anchor=compact_anchor(c,r.model,"UI/Foundations/Components/Settings/Widgets/SettingWrapper_Widget",172.f,false);
    }
    plain_label(c,c.field(r.model,0x58c9d354),title);
    if(r.tile.handle) {
        const auto style=c.path(r.tile,{0x0fb0d794,0x8cc042ef});
        const auto desired=record_ref(c,"ButtonStyleList/TileButton.RoughPartial.Default");
        const auto current=read<Ref>(c.address(style));
        if(current.record!=desired.record || current.handle!=desired.handle)c.set(style,desired);
    }
    if(selectable) {
        const auto choices=c.field(r.selector,0x760fc12b);
        const auto current=read<Address>(c.address(c.field(choices,0xf2c90867)));
        const auto count=current?read<std::uint32_t>(current-4)&0x7fffffff:0;
        if(count!=r.choices.size()) {
            c.array(c.field(choices,0xf2c90867),r.choices);
            const auto index=players?s.max_players-1:s.seconds/10-1;
            c.set(c.field(choices,0x55511280),static_cast<int>(index));
            c.set(c.field(choices,0x783c4b49),r.choices[index]);
        }
        const auto index=read<int>(c.address(c.field(choices,0x55511280)));
        if(index>=0 && static_cast<std::size_t>(index)<r.choices.size()) {
            if(players)s.max_players=static_cast<unsigned>(index)+1;
            else s.seconds=static_cast<unsigned>(index+1)*10;
        }
    } else {
        const auto control=reference(c,read<Widget>(c.address(c.field(r.model,content))).data);
        plain_label(c,control,value);
    }
    shown.push_back(id);
}
void penalty_row(const Context& c,std::vector<std::string>& shown) {
    auto& s=state(); auto [entry,fresh]=s.rows.try_emplace("penalty-stamps"); auto& r=entry->second;
    if(fresh) {
        r.model=make(c,setting_schema);
        const auto label=c.field(r.model,0x58c9d354); c.copy(label,c.address(s.source_label));
        label_styles(c,label,"TextStylesList/H64-XBold_White","TextStylesList/H64-XBold_White");
        plain_label(c,label,"Penalties");
        c.set(c.field(r.model,0xc9bf367d),record_ref(c,"SettingWrapperStylesList/Horizontal.WithDivider"));
        c.set(c.field(r.model,0xf3d74cdd),0);
        r.selector=make(c,linear_list);
        c.array(c.field(r.selector,rows_field),std::vector<Ref>{});
        c.array(c.field(r.selector,0xf2c90867),std::vector<Handle>{});
        c.set(c.field(r.selector,0x55ca89f4),asset(c,"UI/Foundations/Components/Lists/Shared/AnchoredContentPresenterListItem_Widget"));
        c.set(c.field(r.selector,0x64b9a9e8),0);
        c.set(c.field(r.selector,0x4cb61cac),0); // Horizontal stamps.
        c.set(c.field(r.selector,0x19ff199a),108.f);
        c.set(c.field(r.selector,0xebca7354),20.f);
        c.set(c.field(r.selector,0x4554761c),true);
        c.set(c.field(r.selector,0x8bbf7d67),false);
        for(const auto* letter:{"1","U","P"}) {
            auto stamp=make(c,button);
            const auto text=c.field(stamp,0x58c9d354); c.copy(text,c.address(s.source_label));
            plain_label(c,text,letter);
            label_styles(c,text,"TextStylesList/H64-XBold_Black","TextStylesList/H64-XBold_Black");
            c.set(c.field(text,0x3d8639d0),2);
            c.set(c.path(stamp,{0x0fb0d794,0x8cc042ef}),record_ref(c,"ButtonStyleList/ButtonStyle.CTA.Default"));
            const auto anchor=compact_anchor(c,stamp,"UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget",108.f,false);
            c.set(c.field(anchor,0x1cff7243),std::array<float,2>{108.f,108.f});
            // A new anchor has stretch axes. Fixed measures must be paired
            // with a non-stretch axis or the three letters cover each other.
            for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
                const auto layout=c.field(anchor,axis);
                c.set(c.field(layout,0x99bbed5a),.5f); c.set(c.field(layout,0x49943bed),.5f);
                c.set(c.field(layout,0xfec5e0b1),.5f); c.set(c.field(layout,0xd01f4a21),1.f);
            }
            r.choices.push_back(anchor.handle);
        }
        c.set(c.field(r.model,content),Widget{asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),{0,r.selector.handle}});
        r.anchor=compact_anchor(c,r.model,"UI/Foundations/Components/Settings/Widgets/SettingWrapper_Widget",144.f,false);
    }
    const auto mounted=read<Address>(c.address(c.field(r.selector,0xf2c90867)));
    if(!mounted || (read<std::uint32_t>(mounted-4)&0x7fffffff)!=r.choices.size())
        c.array(c.field(r.selector,0xf2c90867),r.choices);
    shown.push_back("penalty-stamps");
}
// Adapt the ACTUAL native setup mounted by SpotBattle. Its stack, anchors,
// transitions, footer and Confirm navigation stay under the game's ownership.
void remove_one_up_rounds(const Context& c,Value root,bool game_mode=false) {
    std::set<Handle> seen;
    std::function<bool(Value,unsigned)> visit=[&](Value value,unsigned depth) -> bool {
        require(depth<32,"1-Up settings graph depth differs.");
        if(!value.handle)return false;
        const auto hash=read<std::uint32_t>(read<Address>(value.type));
        if(hash==setting_schema.hash) {
            // 1-Up drops Spot Battle's rounds; a game mode its rounds, turn timer and player count
            // too (the mode's own panel holds its settings).
            const auto id=c.text(c.path(value,{0x58c9d354,text_field}));
            return id=="ID_LABEL_ROUNDS" ||
                (game_mode && (id=="ID_LABEL_TURNTIMER" || id=="ID_LABEL_DURATION" || id=="ID_SETTINGS_PLAYERS"));
        }
        if(!seen.insert(value.handle).second)return false;
        struct Unvisit { std::set<Handle>& seen;Handle handle;~Unvisit(){seen.erase(handle);} } unvisit{seen,value.handle};
        if(hash==0x62088281) {
            const auto ref=read<Ref>(c.address(value));
            return ref.handle && visit(reference(c,ref),depth+1);
        }
        if(kind(value.type)==4) {
            unsigned n{},stride{};const auto old=c.array(value,128,n,stride);
            std::vector<std::byte> kept;
            for(unsigned i=0;i<n;++i)if(!visit(c.element(value,i),depth+1))
                kept.insert(kept.end(),old.begin()+i*stride,old.begin()+(i+1)*stride);
            if(kept.size()!=old.size())c.array(value,kept,static_cast<unsigned>(kept.size()/stride));
            return false; // Removed an individual row, not its enclosing list.
        }
        bool remove=false;
        if(kind(value.type)==2) {
            const auto meta=read<Address>(value.type),fields=read<Address>(meta+0x60);
            const auto count=read<std::uint16_t>(meta+0x2a);require(count<=128,"1-Up settings field count differs.");
            for(unsigned i=0;i<count;++i) {
                const auto key=read<std::uint32_t>(fields+i*24);
                if(key==0x8cc042ef || key==0xa1e84eb1 || key==0x25189231 || key==0xf94d8cc6 ||
                   key==0x8a3e2159 || key==0x54c9abee || key==0xd78d240b || key==0x85b8c3e6 || key==655725074U)continue;
                const auto child=c.field(value,key);
                if(key==0xf2c90867 && kind(child.type)==4) {
                    unsigned n{},stride{};const auto old=c.array(child,128,n,stride);
                    require(stride==sizeof(Handle),"1-Up setting handle list differs.");
                    std::vector<Handle> kept;
                    for(unsigned j=0;j<n;++j) {
                        Handle handle{};std::memcpy(&handle,old.data()+j*stride,stride);
                        if(!handle || !visit({handle,c.type_of(handle)},depth+1))kept.push_back(handle);
                    }
                    if(kept.size()!=n)c.array(child,kept);
                } else if(kind(child.type)==2 || kind(child.type)==4)remove=visit(child,depth+1)||remove;
            }
        }
        return remove;
    };
    visit(root,0);
}
void official_setup(const Context& c) {
    auto& s=state();
    // The same native setup dressed for a game mode while its flag is placed.
    const bool game_mode=one_up::flag_for_mode() && one_up::flag_placement_active();
    std::string mode_title, mode_line;
    if(game_mode) {
        const auto menu=modes::menu_view();
        if(const auto mode=modes::parse_mode(menu.mode)) {
            mode_title=upper_text(std::string(modes::mode_name(*mode)));
            mode_line=std::string(modes::mode_summary(*mode));
        }
    }
    if(!one_up::native_setup_active() && !game_mode) {
        restore_official(c);
        s.setup_open.store(false); return;
    }
    if(s.reset_setup.exchange(false))s.seconds=20;
    s.setup_open.store(true);
    for(const auto& root:c.roots({panel_schema.hash})) {
        if(root.model.handle==s.panel.handle)continue;
        const auto header_slot=c.field(root.model,0xc0f1b449);
        auto header=reference(c,read<Widget>(c.address(header_slot)).data);
        if(!header.handle || read<std::uint32_t>(read<Address>(header.type))!=header_schema.hash)continue;
        auto title=c.field(header,0x00f3b15e);
        const auto name=c.text(c.field(title,text_field));
        if(!s.official_panels.contains(root.model.handle) && name!="Spot Battle" && name!="SPOT BATTLE" && name!="ID_ACTIVITY_SPOTBATTLE_TITLE")continue;
        const bool fresh=!s.official_panels.contains(root.model.handle);
        if(fresh) {
            // Finish the private graph before changing any live presenter. A
            // missing native dependency must leave the stock panel intact and
            // must not mark a half-built panel as a successful 1-Up adaptation.
            PrivateUiGraph graph{c};
            std::vector<std::pair<Value,Value>> copies;
            for(const auto key:{0xc0f1b449U,0x43967eceU,0xbfc64535U}) {
                const auto slot=c.field(root.model,key);
                const auto copy=graph.clone(key==0x43967eceU?Ref{0,slot.handle}:read<Widget>(c.address(slot)).data);
                if(key==0x43967eceU)remove_one_up_rounds(c,copy,game_mode);
                copies.emplace_back(slot,copy);
                if(key==0xc0f1b449U)header=copy;
            }
            for(const auto& [slot,copy]:copies)remember_official(c,slot);
            try {
                for(const auto& [slot,copy]:copies) {
                    if(slot.type==copy.type)c.copy(slot,c.address(copy));
                    else {
                        auto current=read<Widget>(c.address(slot));current.data={0,copy.handle};c.set(slot,current);
                    }
                    s.official_contents.emplace(slot.handle,copy);
                    s.official_slot_panels.emplace(slot.handle,root.model.handle);
                }
                s.official_headers.emplace(root.model.handle,header);
                s.official_panels.insert(root.model.handle);
            } catch(...) {
                for(const auto& [slot,copy]:copies) {
                    try {c.copy(slot,c.address(s.official_originals.at(slot.handle).second));}catch(...) {}
                    s.official_contents.erase(slot.handle);s.official_slot_panels.erase(slot.handle);
                    s.official_originals.erase(slot.handle);
                }
                s.official_headers.erase(root.model.handle);s.official_panels.erase(root.model.handle);
                throw;
            }
            title=c.field(header,0x00f3b15e);
        } else if(const auto saved=s.official_headers.find(root.model.handle);saved!=s.official_headers.end() && header.handle!=saved->second.handle) {
            auto mounted=read<Widget>(c.address(header_slot));mounted.data={0,saved->second.handle};
            c.set(header_slot,mounted);header=saved->second;title=c.field(header,0x00f3b15e);
        }
        if(fresh) logging::log(logging::Level::info,logging::Channel::ui,
            "1-Up: using original native Throwdown setup {:#x}.",root.model.handle);
        for(const auto key:{0xbfc64535U}) {
            const auto slot=c.field(root.model,key);const auto copy=s.official_contents.at(slot.handle);
            auto current=read<Widget>(c.address(slot));
            if(current.data.handle!=copy.handle) {current.data={0,copy.handle};c.set(slot,current);}
        }
        plain_label(c,title,game_mode?mode_title:std::string("1-UP"));
        if(game_mode) {
            // A game mode keeps the panel's own mark, the native Confirm and its flag placement.
            for(const auto notice:ui_models(c,c.field(root.model,0x43967ece),notice_schema.hash))
                plain_label(c,c.field(notice,0x94703efc),mode_line);
            continue;
        }
        // The blue art is a RimeTexture, while ActivityDisplay's framed icon
        // expects a RimeImage. Use the native Texture widget for this slot;
        // optional art must never block the title or native controls.
        try {
            const auto blueprint=asset(c,"UI/Foundations/Components/Media/Icons/Texture_Widget");
            if(!s.badge.handle) {
                s.badge=make(c,texture_schema);
                publish_texture(c,std::array{c.field(s.badge,0x6e469d2a)},"UI/ReSkate/OneUp/img_OneUp_Blue_1024");
                c.set(c.field(s.badge,0xf524c836),288.f);c.set(c.field(s.badge,0x6fbd254e),288.f);
                c.set(c.field(s.badge,0x81c94d8a),false);c.set(c.field(s.badge,0xd1997d0c),false);
                c.set(c.field(s.badge,0x190e039c),false);
            }
            const auto icon=c.field(header,content);
            const auto current=read<Widget>(c.address(icon));
            if(current.data.handle!=s.badge.handle)c.set(icon,Widget{blueprint,{0,s.badge.handle}});
        } catch(const std::exception& e) {
            if(fresh)logging::log(logging::Level::warning,logging::Channel::ui,"1-Up optional setup badge: {}",e.what());
        }
        const auto settings=ui_models(c,c.field(root.model,0x43967ece),setting_schema.hash);
        const auto notices=ui_models(c,c.field(root.model,0x43967ece),notice_schema.hash);
        const auto buttons=ui_models(c,c.field(root.model,0xbfc64535),button.hash);
        for(const auto setting:settings) {
            const auto label=c.field(setting,0x58c9d354);
            const auto id=c.text(c.field(label,text_field));
            const bool players=id=="ID_SETTINGS_PLAYERS";
            const bool turn=id=="ID_LABEL_TURNTIMER" || id=="ID_LABEL_DURATION";
            if(!players && !turn)continue;
            const auto presentation=c.field(setting,content);
            auto slider=reference(c,read<Widget>(c.address(presentation)).data);
            if(!slider.handle || read<std::uint32_t>(read<Address>(slider.type))!=0xcdc749b2)continue;
            const bool first=!s.official_sliders.contains(presentation.handle);
            if(first) {
                // The native widget still handles input, focus and rendering.
                // Its shared Spot Battle setting must not receive 1-Up values.
                clear_value(c,c.field(slider,2243478502U));
                clear_value(c,c.field(slider,655725074U));
                s.official_sliders.emplace(presentation.handle,slider);
            } else {
                const auto saved=s.official_sliders.at(presentation.handle);
                if(slider.handle!=saved.handle) {
                    auto mounted=read<Widget>(c.address(presentation));mounted.data={0,saved.handle};c.set(presentation,mounted);
                }
                slider=saved;
            }
            const auto range=c.field(slider,0x170bebf7), value=c.field(slider,0x5a547e87);
            const auto step=c.field(slider,0xb47e6cec);
            const float minimum=players?1.f:10.f, maximum=players?6.f:120.f, increment=players?1.f:10.f;
            if(read<std::array<float,2>>(c.address(range))!=std::array<float,2>{minimum,maximum})
                c.set(range,std::array<float,2>{minimum,maximum});
            if(read<float>(c.address(step))!=increment)c.set(step,increment);
            c.set(c.field(slider,264787447U),true); // TrimWholeValues
            c.set(c.field(slider,3516482810U),true); // RoundToNearestWhole
            if(first)c.set(value,native_slider_position(players?s.max_players:s.seconds,unsigned(minimum),unsigned(maximum)));
            const auto normalized=read<float>(c.address(value));
            const unsigned selected=native_slider_choice(normalized,unsigned(minimum),unsigned(maximum),unsigned(increment));
            const auto snapped=native_slider_position(selected,unsigned(minimum),unsigned(maximum));
            if(!std::isfinite(normalized) || std::abs(normalized-snapped)>.00001f)c.set(value,snapped);
            if(players)s.max_players=selected;else s.seconds=selected;
        }
        one_up::set_native_setup_options(s.seconds,s.max_players);
        for(const auto notice:notices) {
            const auto label=c.field(notice,0x94703efc);
            plain_label(c,label,"Add up every landed trick before time runs out.\nBeat the previous total. Three penalties and you're out!");
        }
        // Keep the native Confirm input and its location initializer. The
        // placement observer hands off to the flag as soon as it completes.
        for(const auto b:buttons) {
            const auto action=c.path(b,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
            const auto nav=c.text(c.field(action,0xb1e14cd6));
            if(nav=="ThrowdownPlacement" || nav=="Throwdown_SKATE_Start")continue;
            if(nav!="AdvancedParameters")continue;
            c.text(c.field(action,0xb1e14cd6),"ThrowdownerExit");
            const auto label=c.field(b,0x58c9d354);plain_label(c,label,"BACK");
        }
    }
}
void stock_solo_controls(const Context& c) {
    auto& s=state();
    for(const auto& root:c.roots({panel_schema.hash})) {
        if(s.official_panels.contains(root.model.handle) || root.model.handle==s.panel.handle)continue;
        const auto header=reference(c,read<Widget>(c.address(c.field(root.model,0xc0f1b449))).data);
        if(!header.handle || read<std::uint32_t>(read<Address>(header.type))!=header_schema.hash)continue;
        const auto title=c.text(c.path(header,{0x00f3b15e,text_field}));
        const bool stock=title=="S.K.A.T.E." || title=="ID_ACTIVITY_SKATE_TITLE" ||
            title=="Spot Battle" || title=="SPOT BATTLE" || title=="ID_ACTIVITY_SPOTBATTLE_TITLE" ||
            title=="Skate Jam" || title=="SKATE JAM" || title=="ID_ACTIVITY_JAMSESSION_TITLE";
        if(!stock)continue;
        for(const auto setting:ui_models(c,c.field(root.model,0x43967ece),setting_schema.hash)) {
            if(c.text(c.path(setting,{0x58c9d354,text_field}))!="ID_SETTINGS_PLAYERS")continue;
            const auto slot=c.field(setting,content);
            auto slider=reference(c,read<Widget>(c.address(slot)).data);
            if(!slider.handle || read<std::uint32_t>(read<Address>(slider.type))!=0xcdc749b2)continue;
            if(const auto existing=s.solo_sliders.find(slot.handle);existing!=s.solo_sliders.end() && existing->second.handle==slider.handle)continue;
            const auto old_range=read<std::array<float,2>>(c.address(c.field(slider,0x170bebf7)));
            if(!std::isfinite(old_range[0]) || !std::isfinite(old_range[1]) || old_range[0]<1 || old_range[1]<2 || old_range[1]>32)continue;
            const auto maximum=static_cast<unsigned>(old_range[1]);
            const auto selected=native_slider_choice(read<float>(c.address(c.field(slider,0x5a547e87))),static_cast<unsigned>(old_range[0]),maximum,1);
            // This change is intentionally shared by all three stock modes.
            // Keep the original model identity so its authored backend binding
            // reads the same slider the native widget changes.
            c.set(c.field(slider,0x170bebf7),std::array<float,2>{1.f,old_range[1]});
            c.set(c.field(slider,0xb47e6cec),1.f);
            c.set(c.field(slider,264787447U),true);c.set(c.field(slider,3516482810U),true);
            c.set(c.field(slider,0x5a547e87),native_slider_position(selected,1,maximum));
            s.solo_sliders[slot.handle]=slider;
            logging::log(logging::Level::info,logging::Channel::ui,"Throwdowns: native {} Players slider supports 1-{}.",title,maximum);
        }
    }
}
void retire_solo_pending_queue(const Context& c) {
    if(!native_solo_throwdown_menu_cleanup_pending())return;
    unsigned found{};
    // Live capture of the locked wheel after confirmed Spot Battle solo Quit:
    // dependencies were entitlement=1, IsInQueueState=false,
    // IsPendingQueue=true, ThrowdownerDisabledByRule_RefCount=0.
    // Publish the actual dependency, rather than bypassing IsThrowdownerDisabled
    // or erasing unrelated activity/entitlement restrictions.
    for(const auto& root:c.roots({0x6ee228a8})) {
        if(c.text(c.path(root.model,{0x58c9d354,text_field}))!="ID_THROWDOWNS")continue;
        const auto dependencies=c.path(root.model,{0x67212c85,0x4c09193f,0x8a56e2ac});
        unsigned count{},stride{};c.array(dependencies,8,count,stride);
        if(count!=4 || stride!=sizeof(Ref))continue;
        const auto ref=read<Ref>(c.address(c.element(dependencies,2)));
        if(!ref.record || string(read<Address>(ref.record+0x28),128)!="TD_Page_ContentResources/IsPendingQueue")continue;
        const auto pending=reference(c,ref);
        if(!pending.handle || kind(pending.type)!=10 || size(pending.type)!=1)continue;
        const bool was_pending=read<bool>(c.address(pending));
        if(was_pending)c.set(pending,false);
        ++found;
        logging::log(logging::Level::info,logging::Channel::ui,
            "Throwdowns: confirmed solo exit published IsPendingQueue {} -> false on the native wheel (model {:#x}).",
            was_pending,pending.handle);
    }
    if(found)native_solo_throwdown_menu_cleaned();
}
void footer_action(const Context& c,Value button_value,const std::string& text,std::string command,std::string argument={}) {
    plain_label(c,c.field(button_value,0x58c9d354),text);
    auto action=c.path(button_value,{0x0fb0d794,0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
    c.set(c.field(action,0x9c76b86c),callback(c,std::move(command),std::move(argument)));
    c.text(c.field(action,0xb1e14cd6),"");
    if(button_value.handle==state().footer_confirm.handle) {
        // PageActions is an array of native ButtonAction values. Like the
        // original Throwdown Confirm, this action is available while a
        // selector has focus; it does not require navigating into the footer.
        auto page_action=c.path(state().confirm_action,{0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
        c.copy(page_action,c.address(action));
        const auto input_asset=state().primary_input;
        c.set(c.field(page_action,0x3d00de27),input_asset);
        c.set(c.field(page_action,0x0169c2db),input_asset);
        const auto actions=c.field(state().page,0xd2118ca8);
        unsigned count{},stride{};
        c.array(actions,8,count,stride);
        require(stride==page_action_schema.size,"1-Up page action schema differs.");
        bool publish=count!=1;
        if(!publish) {
            const auto mounted=c.path(c.element(actions,0),{0xa704272a,0xc52416ef,0xa704272a,0xa2b71c93});
            publish=read<Address>(c.address(c.field(mounted,0x9c76b86c)))!=read<Address>(c.address(c.field(page_action,0x9c76b86c)));
        }
        if(publish) {
            std::vector<std::byte> bytes(page_action_schema.size);
            require(memory::peek_bytes(c.address(state().confirm_action),bytes.data(),bytes.size()),"1-Up Confirm action unavailable.");
            c.array(actions,bytes,1);
        }
    }
}
void session_marker_controls(const Context& c,bool release) {
    auto& originals=state().marker_originals;
    if(release || !one_up::restricts_session_markers()) {
        for(const auto& [handle,saved]:originals)
            if(c.type_of(handle)==saved.first.type) c.set(saved.first,saved.second);
        originals.clear();
        return;
    }
    // SessionMarkerDataModel is the game's keyboard/menu binding. The
    // skater-intent predicates separately guard controller marker actions.
    for(const auto& root:c.roots({4200505362U}))
        for(const auto [hash,disabled]:std::array{
                std::pair{2627953652U,true},std::pair{753092917U,false},std::pair{1976179319U,false}}) {
            const auto field=c.field(root.model,hash);
            require(size(field.type)==sizeof(bool),"Native session marker field ABI differs.");
            const auto current=read<bool>(c.address(field));
            originals.try_emplace(field.handle,std::pair{field,current});
            if(current!=disabled)c.set(field,disabled);
        }
}
// The waiting card a native flag brings, for a 1-Up match or a game mode's flag: its title, the
// line under it, the player count, the countdown and what Start and Leave do.
struct WaitingCard {
    bool active{}, lobby{}, ticking{}, one_up{}, can_start{};
    std::string title, message, count, start_label, leave_label;
    std::string start_command, start_argument, leave_command, leave_argument;
    std::uint32_t remaining{}, countdown_total{};
};
WaitingCard waiting_card() {
    WaitingCard w;
    const auto v=one_up::view();
    if(v.state.match && v.state.phase!=one_up::Phase::cancelled) {
        w.active=w.one_up=true;
        w.title="1-UP";
        w.lobby=v.state.phase==one_up::Phase::lobby;
        w.ticking=v.state.phase==one_up::Phase::countdown;
        w.remaining=v.remaining; w.countdown_total=v.state.config.countdown_ms;
        const auto ready=std::count_if(v.state.players.begin(),v.state.players.end(),[](const auto& p){return p.connected && p.ready;});
        if(w.lobby) w.message="Waiting for players  "+std::to_string(ready)+" / "+std::to_string(v.state.config.max_players)+" ready";
        else if(w.ticking) w.message="Starting in "+std::to_string((v.remaining+999)/1000);
        else if(v.state.phase==one_up::Phase::finished) w.message=v.name(v.state.winner)+" wins!";
        else w.message=v.name(v.state.active)+"  |  Target "+std::to_string(static_cast<unsigned>(v.state.target));
        const auto connected=std::count_if(v.state.players.begin(),v.state.players.end(),[](const auto& p){return p.connected;});
        w.count=std::to_string(connected)+"/"+std::to_string(v.state.config.max_players);
        w.leave_label=one_up::restricts_session_markers()?"QUIT MATCH":"LEAVE"; w.leave_command="oneup"; w.leave_argument="leave";
        w.start_label=w.lobby?"START":"1-UP"; w.start_command=w.lobby?"oneup":"open"; w.start_argument=w.lobby?"start":"";
        return w;
    }
    if(!one_up::flag_for_mode()) return w;
    const auto menu=modes::menu_view();
    if(!menu.in_game) return w;
    const auto mode=modes::parse_mode(menu.mode);
    w.active=true;
    w.title=mode?upper_text(std::string(modes::mode_name(*mode))):std::string("GAME");
    w.lobby=menu.phase<=1;
    w.ticking=menu.phase==2;
    const auto m=modes::native_match();
    w.remaining=m.active?m.remaining_ms:0; w.countdown_total=m.active?m.countdown_ms:5000;
    if(w.lobby) w.message=menu.missing.empty()?"Waiting for skaters  "+std::to_string(menu.players)+" in":menu.missing;
    else if(w.ticking) w.message="Starting in "+std::to_string((w.remaining+999)/1000);
    else if(menu.phase==4) w.message=m.winner.empty()?std::string("Game over"):m.winner+" wins!";
    else w.message=m.headline;
    w.count=std::to_string(menu.players);
    w.can_start=menu.leading && w.lobby;
    w.start_label=w.can_start?"START":"GAME"; w.start_command="mode"; w.start_argument=w.can_start?"start":"";
    w.leave_label=menu.leading?"END GAME":"LEAVE"; w.leave_command="mode"; w.leave_argument=menu.leading?"stop":"leave";
    return w;
}
void waiting_hud(const Context& c) {
    auto& s=state(); const auto w=waiting_card();
    // Destroying the flag must not restore a lingering Spot Battle banner
    // during 1-Up. The private presentation belongs to the match until exit.
    if(!w.active) {
        for(const auto& [handle,presentation]:s.waiting_presentations)
            if(c.type_of(handle)==presentation.first.type) {
                const auto current=read<Widget>(c.address(presentation.first)).data.handle;
                const bool owned=std::any_of(s.waiting_originals.begin(),s.waiting_originals.end(),[&](const auto& entry) {
                    return current==entry.second.handle || current==c.field(entry.second,0xc13bcfcd).handle;
                });
                if(owned)c.copy(presentation.first,c.address(presentation.second));
            }
        s.waiting_presentations.clear();
        s.waiting_originals.clear(); s.waiting_widgets.clear(); s.waiting_counts.clear(); return;
    }
    // The game's queue creates and places this SecondaryNotification itself.
    // Keep its layout, transitions and input prompts; only supply 1-Up data.
    // The live HUD is in the gameplay asset domain, which need not be a parent
    // of the card's domain. Resolve its registered type directly.
    constexpr std::uint32_t hash=273922540U; // NotificationViewModel
    // Authored Notification defaults and other challenge queues are registered
    // roots too. Only adapt a notification actually mounted in the HUD stack.
    std::map<Handle,Value> mounted_notifications;
    for(const auto& root:c.roots({0xa842b4d0})) {
        const auto notifications=c.path(root.model,{0xd9f424d1,items});
        unsigned n{},stride{};c.array(notifications,128,n,stride);
        for(unsigned i=0;i<n;++i) {
            const auto presentation=c.field(c.element(notifications,i),content);
            const auto mounted=mounted_notification(presentation.handle,[&](Handle handle) {
                const Value slot{handle,c.type_of(handle)};
                const auto saved=s.waiting_presentations.find(handle);
                const auto current=read<Widget>(c.address(saved==s.waiting_presentations.end()?slot:saved->second.second));
                const auto type=current.data.handle?c.type_of(current.data.handle):0;
                if(!type)return NotificationMountNode{};
                const auto hash=read<std::uint32_t>(read<Address>(type));
                const auto inner=hash==0xffb83e48?c.field({current.data.handle,type},content).handle:0;
                return NotificationMountNode{current.data.handle,hash,inner};
            });
            if(mounted)mounted_notifications.emplace(mounted->model,Value{mounted->presentation,c.type_of(mounted->presentation)});
        }
    }
    const auto candidates=c.roots({hash});
    if(s.waiting_widgets.empty() && GetTickCount64()>=s.waiting_diagnostic) {
        s.waiting_diagnostic=GetTickCount64()+10000;
        logging::log(logging::Level::info,logging::Channel::ui,"1-Up: looking for native queue HUD type {:#x}, {} root models, {} mounted notifications.",hash,candidates.size(),mounted_notifications.size());
    }
    for(const auto& root:candidates) {
        const auto source=root.model;
        if(!mounted_notifications.contains(source.handle))continue;
        if(!s.waiting_widgets.contains(source.handle)) {
            if(!one_up::flag_registration_owned())continue; // Never acquire another challenge's queue.
            const auto title=c.text(c.path(source,{0x1c1d36ea,0x44688629,text_field}));
            if(title!="Spot Battle" && title!="SPOT BATTLE" && title!="ID_ACTIVITY_SPOTBATTLE_TITLE")continue;
            if(c.text(c.path(source,{0xd31e2918,0x58c9d354,text_field}))!="ID_START" ||
               c.text(c.path(source,{0xc13bcfcd,0x58c9d354,text_field}))!="ID_LEAVE")continue;
            const auto presentation=mounted_notifications.at(source.handle);
            const auto original=make(c,presenter);c.copy(original,c.address(presentation));
            s.waiting_presentations.emplace(presentation.handle,std::pair{presentation,original});
            const auto copy=clone_widget_tree(c,presentation);
            s.waiting_originals.emplace(source.handle,copy);
            s.waiting_widgets.insert(source.handle);
            logging::log(logging::Level::info,logging::Channel::ui,"1-Up: private native queue HUD bound, source {:#x}, model {:#x}.",source.handle,copy.handle);
        }
        const auto model=s.waiting_originals.at(source.handle);
        const auto start=c.field(model,0xd31e2918),leave=c.field(model,0xc13bcfcd);
        clear_value(c,c.field(start,0x67212c85));
        const auto heading=c.field(model,0x1c1d36ea);
        plain_label(c,c.field(heading,0x44688629),w.title);
        plain_label(c,c.field(heading,0x00f3b15e),w.message);
        // Start/Leave must remain usable even if optional icon loading is late.
        footer_action(c,leave,w.leave_label,w.leave_command,w.leave_argument);
        footer_action(c,start,w.start_label,w.start_command,w.start_argument);
        // A game mode's card keeps the game's own Throwdown mark; 1-Up shows its blue badge.
        if(w.one_up) {
            if(!s.waiting_badge.handle) {
                s.waiting_badge=make(c,texture_schema);
                c.set(c.field(s.waiting_badge,0xf524c836),96.f);
                c.set(c.field(s.waiting_badge,0x6fbd254e),96.f);
            }
            publish_texture(c,std::array{c.field(s.waiting_badge,0x6e469d2a)},"UI/ReSkate/OneUp/img_OneUp_Blue_1024");
            c.set(c.field(heading,content),Widget{asset(c,"UI/Foundations/Components/Media/Icons/Texture_Widget"),{0,s.waiting_badge.handle}});
        }
        // The stock queue's decorator displays its backend limit (10). Keep
        // the native green participant icon but bind a private count model to
        // the actual 1-Up roster and selected player limit.
        auto& participants=s.waiting_counts[source.handle];
        const auto decorator=c.field(model,0xe01931be);
        if(!participants.handle)participants=clone_widget_model(c,decorator);
        auto participant_widget=read<Widget>(c.address(decorator));
        if(participant_widget.data.handle!=participants.handle) {
            participant_widget.data={0,participants.handle};
            c.set(decorator,participant_widget);
        }
        plain_label(c,c.field(participants,0x58c9d354),w.count);
        // CountdownViewModel explicitly supports an outside time source when
        // DoCountdown is false. Keep the authored timer artwork/animation and
        // feed the synchronized match clock, never the native S.K.A.T.E. clock.
        if(!s.waiting_countdown.handle) {
            const auto original=read<Ref>(c.address(c.field(model,0x0750d51d)));
            const auto timer_type=original.handle?c.type_of(original.handle):(original.record?read<Address>(original.record+0x18):0);
            require(timer_type!=0,"Native waiting timer is not ready.");
            const auto data=original.handle?c.address({original.handle,timer_type}):read<Address>(original.record+0x20);
            s.waiting_countdown=make(c,{1783008579U,static_cast<std::uint16_t>(size(timer_type))},timer_type);
            c.copy(s.waiting_countdown,data);
        }
        const bool ticking=w.ticking;
        const auto timer=s.waiting_countdown;
        c.set(c.field(timer,1883000211U),false); // DoCountdown: externally driven
        c.set(c.field(timer,376517051U),ticking); // ForceActive
        c.set(c.field(timer,1636171836U),ticking); // Running
        c.set(c.field(timer,3239084782U),true); // UpdateText
        native_timer_seconds(c,timer,1288039561U,w.countdown_total);
        native_timer_seconds(c,timer,247769280U,ticking?w.remaining:0);
        c.text(c.field(timer,378491941U),ticking?std::to_string((w.remaining+999)/1000):"");
        c.set(c.field(model,0x0750d51d),Ref{0,timer.handle});
    }
    // The registration keeps the world flag alive. Its large waiting card is
    // only needed in the lobby; during the match retain the same native Leave
    // button in the same notification slot. Never pop the backend queue.
    const bool compact=!w.lobby;
    for(const auto& [notification,presentation]:mounted_notifications) {
        const auto current=read<Widget>(c.address(presentation));
        if(!s.waiting_widgets.contains(notification))continue;
        const auto model=s.waiting_originals.at(notification);
        const auto leave=c.field(model,0xc13bcfcd);
        auto replacement=read<Widget>(c.address(s.waiting_presentations.at(presentation.handle).second));
        replacement.data={0,model.handle};
        if(compact)replacement={asset(c,"UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget"),{0,leave.handle}};
        if(current.data.handle!=replacement.data.handle)c.set(presentation,replacement);
    }
}
void mount_hud(const Context& c,Value model,Address blueprint,Value& anchor,Value& item,int id,bool center) {
    auto& s=state();
    const auto roots=c.roots({0xe4c44873}); // HudViewModel, including authored defaults
    std::vector<HudRootCandidate> candidates;
    for(const auto& root:roots) {
        const auto stack=c.field(root.model,0x44736c13); // FullAspectRatioStack
        const bool active=read<bool>(c.address(c.field(stack,0x369babfe)));
        unsigned populated{};
        // Never let 1-Up's own appended items make an unmounted template look
        // live. These are independent, game-owned gameplay HUD stacks.
        for(const auto field:{0xa7b6b40fU,0xee58682eU,0x0f3c11c2U}) {
            unsigned count{},stride{};
            c.array(c.path(root.model,{field,items}),64,count,stride);
            populated+=count!=0;
        }
        candidates.push_back({root.model.handle,active,populated});
    }
    const auto selected=mounted_hud_root(candidates,s.hud_root.handle);
    require(selected!=0,"Native gameplay HUD is not mounted.");
    if(selected!=s.hud_root.handle || !s.hud_stack.handle) {
        publish_hud_items(c,false,false); // Detach from a retired root before rebinding.
        const auto root=std::find_if(roots.begin(),roots.end(),[&](const auto& r){return r.model.handle==selected;});
        s.hud_root=root->model;
        s.hud_stack=c.field(s.hud_root,0x44736c13);
        logging::log(logging::Level::info,logging::Channel::ui,
            "1-Up: attached to live gameplay HUD {:#x}; authored defaults excluded.",selected);
    }
    const auto anchor_widget=asset(c,"UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget");
        if(anchor.handle && item.handle)return;
        const auto next_anchor=make(c,anchor_schema);
        c.set(c.field(next_anchor,widget),Widget{blueprint,{0,model.handle}});
        c.set(c.field(next_anchor,0x1cff7243),std::array<float,2>{center?700.f:1100.f,center?480.f:900.f});
        c.set(c.field(next_anchor,0x144aee01),false);
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const bool vertical=axis==0x185a5736U;
            const auto layout=c.field(next_anchor,axis);
            const float at=center?(vertical?.38f:.5f):(vertical?.12f:0.f);
            const float offset=!center && !vertical?110.f:0.f;
            c.set(c.field(layout,0x99bbed5a),at);c.set(c.field(layout,0x49943bed),at);
            c.set(c.field(layout,0x40bfbff4),offset);c.set(c.field(layout,0x1d233266),offset);
            c.set(c.field(layout,0xfec5e0b1),center?.5f:0.f);c.set(c.field(layout,0xd01f4a21),1.f);
        }
        const auto next_item=make(c,stack_item);
        c.set(c.field(next_item,0x1e95752c),id);
        c.copy(c.field(next_item,0xd78d240b),c.address(c.field(s.hud_stack,0x54c9abee)));
        c.set(c.field(next_item,content),Widget{anchor_widget,{0,next_anchor.handle}});
        c.set(c.field(next_item,0x369babfe),true);
        for(const auto track:{0x8c0583eeU,0xc5c7cc11U}) {
            c.set(c.path(next_item,{track,0x20247753}),1.f);c.set(c.path(next_item,{track,0x2426103f}),1.f);
            c.set(c.path(next_item,{track,0x1e3ce013}),true);
        }
        anchor=next_anchor;item=next_item;
}
// What skate.'s own HUD widgets show: a 1-Up match, or else the local player's game mode (Spot Jam,
// Deathrace, Skate Tag...: Extension/Modes). The same native intro, 3-2-1, score block with its
// clock, elimination stamps or scores, and results board serve both.
struct Feed {
    bool match{}, one_up{};
    std::uint64_t key{};             // the match or game
    std::uint64_t token{};           // changes for each countdown (each 1-Up turn)
    enum class Phase { lobby, countdown, playing, ended } phase{};
    bool positioning{}, practice_done{}, cancelled{};
    std::uint32_t remaining{};       // the countdown's or the clock's time left
    std::uint32_t clock_total{};     // the clock's whole length (0: no clock)
    std::uint32_t results_after{};   // how long after the end the results board replaces the score
    std::string title, tagline, headline, best, mine, results_notice;
    struct Row {
        std::uint64_t id{};
        std::string name, value;     // value: a game mode's score (1-Up shows stamps)
        unsigned penalties{};
        bool out{}, self{}, focus{}, connected = true;
    };
    std::vector<Row> rows;           // the results board's order
    std::uint64_t winner{}, local{};
};
std::string upper_text(std::string text) {
    for (auto& ch : text) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    return text;
}
Feed current_feed() {
    Feed f;
    const auto v = one_up::view();
    if (v.state.match) {
        f.match = f.one_up = true;
        f.key = v.state.match;
        f.local = v.local;
        f.token = v.state.match * 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(v.state.turn) << 32) ^ v.state.active;
        switch (v.state.phase) {
        case one_up::Phase::lobby: f.phase = Feed::Phase::lobby; break;
        case one_up::Phase::countdown: f.phase = Feed::Phase::countdown; break;
        case one_up::Phase::finished:
        case one_up::Phase::cancelled: f.phase = Feed::Phase::ended; break;
        default: f.phase = Feed::Phase::playing; break;
        }
        f.cancelled = v.state.phase == one_up::Phase::cancelled;
        f.positioning = v.positioning;
        f.remaining = v.remaining;
        f.clock_total = v.state.config.turn_ms;
        f.results_after = 4000;
        f.title = "1-UP";
        f.tagline = "Beat the target. Last skater standing wins!";
        const auto points = [](double n) { return std::to_string(static_cast<std::uint32_t>(std::max(0.0, n))); };
        f.best = points(v.state.target);
        f.mine = points(v.state.best);
        f.winner = v.state.winner;
        f.practice_done = v.solo_test && v.state.phase == one_up::Phase::cancelled &&
                          std::any_of(v.state.players.begin(), v.state.players.end(), [](const auto& p) { return p.penalties == 3; });
        f.headline = f.phase == Feed::Phase::ended ? (v.state.winner ? v.name(v.state.winner) + " WINS!" : std::string("1-UP COMPLETE"))
                                                   : "1-UP  /  " + v.name(v.state.active);
        f.results_notice = v.state.winner ? v.name(v.state.winner) + " WINS!" : std::string("PRACTICE COMPLETE");
        for (const auto& p : v.state.players)
            f.rows.push_back({p.id, v.name(p.id), {}, p.penalties, !p.eligible(), p.id == v.local, false, p.connected});
        return f;
    }
    const auto m = modes::native_match();
    if (!m.active) return f;
    f.match = true;
    f.key = f.token = m.key;
    f.phase = m.phase == 2 ? Feed::Phase::countdown : m.phase == 3 ? Feed::Phase::playing : m.phase == 4 ? Feed::Phase::ended : Feed::Phase::lobby;
    f.remaining = m.remaining_ms;
    f.clock_total = m.clock_ms;
    f.results_after = 1200;
    f.title = m.title;
    f.tagline = m.tagline;
    f.headline = f.phase == Feed::Phase::ended && !m.winner.empty() ? upper_text(m.winner) + " WINS!" : m.headline;
    f.best = m.best;
    f.mine = m.mine;
    f.results_notice = m.winner.empty() ? std::string("GAME OVER") : upper_text(m.winner) + " WINS!";
    for (const auto& r : m.rows) {
        f.rows.push_back({r.id, r.name, r.value, 0, r.out, r.self, r.up, true});
        if (r.self) f.local = r.id;
    }
    if (f.phase == Feed::Phase::ended && !m.rows.empty() && !m.winner.empty()) f.winner = m.rows.front().id;
    return f;
}
void native_countdown_hud(const Context& c, const Feed& f) {
    auto& s=state();const auto now=GetTickCount64();
    const bool countdown=f.match && f.phase==Feed::Phase::countdown && !f.positioning;
    const bool fresh_countdown=countdown && s.hud_countdown_token!=f.token;
    // The first part announces the mode using the native Throwdown banner;
    // the final three seconds use TurnStartCountdownHUDRule's actual widget.
    s.hud_intro_visible=countdown && f.remaining>3000;
    if(countdown && !s.hud_intro_visible)s.hud_go_until=now+900;
    const bool show_go=f.match && f.phase==Feed::Phase::playing && s.hud_countdown_token==f.token && now<s.hud_go_until;
    s.hud_countdown_visible=(countdown && !s.hud_intro_visible) || show_go;
    if(s.hud_intro_visible) {
        if(!s.hud_intro.handle) {
            s.hud_intro=record_model(c,"TD_Hud_ContentResources/TD_ActivityBanner");
            c.set(c.field(s.hud_intro,0xf79313be),record_ref(c,"ActivityBannerStylesList/ActivityBannerCenterFullWidthStyle.Throwdown.Default"));
            const auto heading=c.field(s.hud_intro,0xcbc57608);
            const auto authored=record_model(c,"TD_Hud_ContentResources/TD_ActivityBanner_ContentWithTitle");
            c.copy(heading,c.address(authored));
            label_styles(c,c.field(heading,0x44688629),"TextStylesList/D180-Caps_White","TextStylesList/D180-Caps_White");
            if(f.one_up) {
                s.hud_intro_badge=make(c,texture_schema);
                c.set(c.field(s.hud_intro_badge,0xf524c836),256.f);
                c.set(c.field(s.hud_intro_badge,0x6fbd254e),256.f);
                c.set(c.field(heading,content),Widget{asset(c,"UI/Foundations/Components/Media/Icons/Texture_Widget"),{0,s.hud_intro_badge.handle}});
            }
        }
        const auto heading=c.field(s.hud_intro,0xcbc57608);
        plain_label(c,c.field(heading,0x44688629),f.title);
        plain_label(c,c.field(heading,0x00f3b15e),f.tagline);
        if(s.hud_intro_badge.handle)publish_texture(c,std::array{c.field(s.hud_intro_badge,0x6e469d2a)},"UI/ReSkate/OneUp/img_OneUp_Blue_1024");
        mount_hud(c,s.hud_intro,asset(c,"UI/Features/Activities/Blueprints/ActivityBanner_CenterFullWidth_Widget"),s.hud_intro_anchor,s.hud_intro_item,hud_intro_id,true);
        c.set(c.field(s.hud_intro_anchor,0x1cff7243),std::array<float,2>{1920.f,600.f});
    }
    if(s.hud_countdown_visible) {
        if(!s.hud_timer.handle)s.hud_timer=record_model(c,"Activities_HudElementData_ContentResource/Activities_CountdownItemHUD");
        // ActivityCountdownViewModel drives the stock grunge/scribble
        // transitions. CountdownViewModel belongs to the small queue timer
        // and is not the model consumed by the game's turn-start animation.
        plain_label(c,c.field(s.hud_timer,0x58c9d354),show_go?"GO!":std::to_string(native_hud_seconds(f.remaining)));
        c.set(c.field(s.hud_timer,1698918111U),show_go?0.f:std::clamp(static_cast<float>(f.remaining)/3000.f,0.f,1.f));
        mount_hud(c,s.hud_timer,asset(c,"UI/Features/Activities/Widgets/System/ActivityCountdown_Widget"),s.hud_timer_anchor,s.hud_timer_item,hud_countdown_id,true);
        c.set(c.field(s.hud_timer_anchor,0x1cff7243),std::array<float,2>{512.f,512.f});
    }
    if(fresh_countdown) {
        s.hud_countdown_token=f.token;
        logging::log(logging::Level::info,logging::Channel::ui,"Native HUD: {} intro and ActivityCountdown 3-2-1/GO sequence mounted.",f.title);
    }
    publish_hud_items(c,s.hud_ready.load(),s.hud_countdown_visible,s.hud_intro_visible);
}
// The see-through look of our own HUD in the game's own styles: rows without a backing, white
// text with a drop shadow, 60% black score stamps. Each falls back to the opaque stock style.
Ref style_or(const Context& c,const char* wanted,const char* fallback) {
    try { if(const auto ref=record_ref(c,wanted);ref.record) return ref; } catch(...) {}
    return record_ref(c,fallback);
}
void shadow_label(const Context& c,Value label,const char* fallback) {
    const auto style=style_or(c,"TextStylesList/H52-XBold_White_4pxBottomDropShadow",fallback);
    c.set(c.field(label,0xf94d8cc6),style);c.set(c.field(label,0xa1e84eb1),style);c.set(c.field(label,0x25189231),style);
}
void native_score_hud(const Context& c, const Feed& f) {
    auto& s=state();const auto now=GetTickCount64();
    const bool ended=f.phase==Feed::Phase::ended;
    if(!ended)s.hud_ended_at=0;else if(!s.hud_ended_at)s.hud_ended_at=now;
    const bool show=f.match && f.phase!=Feed::Phase::lobby && (!ended || now-s.hud_ended_at<5000);
    if(!show) {publish_hud_items(c,false,s.hud_countdown_visible,s.hud_intro_visible);s.hud_ready.store(false);return;}
    const bool rebuild=!s.hud_initialized || !hud_list_has_items(c,s.hud_content,3) || s.hud_player_ids.size()!=f.rows.size();
    if(rebuild) {
        // These are the same authored records Spot Battle publishes, with
        // private mutable copies. No native style or default is overwritten.
        if(!s.hud_title.handle) {
            s.hud_title=record_model(c,"TD_Hud_ContentResources/TD_HUD_Header_Label");
            label_styles(c,s.hud_title,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
            shadow_label(c,s.hud_title,"TextStylesList/H52-XBold_White");
        }
        if(!s.hud_notice.handle)s.hud_notice=record_model(c,"Activities_HudElementData_ContentResource/Activity_HUD_TargetScore_ContentCombination");
        if(!s.hud_target.handle)s.hud_target=clone_widget_model(c,c.field(s.hud_notice,0xa98391bd));
        if(!s.hud_current.handle)s.hud_current=clone_widget_model(c,c.field(s.hud_notice,0x759c54dd));
        label_styles(c,c.field(s.hud_target,0x58c9d354),"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
        label_styles(c,c.field(s.hud_current,0x94703efc),"TextStylesList/H52-XBold_Black","TextStylesList/H52-XBold_Black");
        if(!s.hud_clock.handle)s.hud_clock=record_model(c,"Activities_HudElementData_ContentResource/A_ActivityCountdown");
        if(!s.hud_rows.handle)s.hud_rows=make(c,linear_list);
        // Retain the native typeface, crown, torn counter background and clock.
        hud_list(c,s.hud_content,{
            {asset(c,"UI/Foundations/Components/Text/Label/Widget/Label_Widget"),{0,s.hud_title.handle}},
            {asset(c,"UI/Foundations/Components/Modals/ContentPresenterCombination_Widget"),{0,s.hud_notice.handle}},
            {asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),{0,s.hud_rows.handle}}},false,60.f,10.f,64.f*static_cast<float>(f.rows.size()),520.f);
        s.hud_initialized=true;
    }
    mount_hud(c,s.hud_content,asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),s.hud_anchor,s.hud_item,hud_score_id,false);
    const auto height=140.f+64.f*static_cast<float>(f.rows.size());
    const auto place=[&](Value anchor,float width,float h,float x,float y,float pivot_x,float pivot_y) {
        c.set(c.field(anchor,0x1cff7243),std::array<float,2>{width,h});
        for(const auto axis:{0xde7d30c7U,0x185a5736U}) {
            const bool vertical=axis==0x185a5736U;const auto layout=c.field(anchor,axis);
            c.set(c.field(layout,0x99bbed5a),vertical?y:x);c.set(c.field(layout,0x49943bed),vertical?y:x);
            c.set(c.field(layout,0x40bfbff4),0.f);c.set(c.field(layout,0x1d233266),0.f);
            c.set(c.field(layout,0xfec5e0b1),vertical?pivot_y:pivot_x);
            c.set(c.field(layout,0xd01f4a21),.85f);
        }
    };
    place(s.hud_anchor,520.f,height,.95f,.82f,1.f,1.f);
    // A game without a clock (S.K.A.T.E. and the other turn games) shows none.
    s.hud_clock_shown=f.clock_total!=0;
    if(s.hud_clock_shown) {
        mount_hud(c,s.hud_clock,asset(c,"UI/Foundations/Components/Countdown/Countdown_Widget"),s.hud_clock_anchor,s.hud_clock_item,hud_clock_id,false);
        place(s.hud_clock_anchor,150.f,64.f,.5f,.93f,.5f,1.f);
    }
    plain_label(c,s.hud_title,f.headline);
    plain_label(c,c.field(s.hud_target,0x58c9d354),f.best.empty()?"-":f.best);
    plain_label(c,c.field(s.hud_current,0x94703efc),f.mine.empty()?"-":f.mine);
    if(s.hud_clock_shown) {
        const auto timer=s.hud_clock;
        c.set(c.field(timer,0x703c4d93),false);
        c.set(c.field(timer,0x167131bb),!ended);c.set(c.field(timer,0x6186003c),!ended);
        c.set(c.field(timer,0xc11082ee),true);
        native_timer_seconds(c,timer,0x4cc5ec89,f.clock_total);
        native_timer_seconds(c,timer,0x0ec4a8c0,f.phase==Feed::Phase::playing?f.remaining:0);
    }
    std::vector<std::uint64_t> ids;
    std::vector<Widget> rows;
    for(const auto& player:f.rows) {
        auto& hud=s.hud_players[player.id];
        // 1-Up: the name and its three 1-U-P stamps. A game mode: the name and one wider stamp
        // holding the player's score.
        const bool stamps=f.one_up;
        const unsigned shown=stamps?3U:1U;
        if(hud.row.handle && hud.stamps_row!=stamps) hud.row={}; // the other kind of row: built again
        if(!hud.row.handle) {
            hud.stamps_row=stamps;
            hud.row=make(c,linear_list);
            if(!hud.name.handle) {
                hud.name=record_model(c,"TD_Hud_ContentResources/TD_HUD_Header_Label");
                label_styles(c,hud.name,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
                c.set(c.field(hud.name,0x87ad624c),2); // Native Label.VerticalAlignment: center.
                c.set(c.field(hud.name,0xc13ba637),false); // FitHeightToContent must not override the row height.
            }
            unsigned index{};
            for(const auto* letter:{"1","U","P"}) {
                auto& stamp=hud.letters[index++];
                if(!stamp.handle)stamp=record_model(c,"Activities_HudElementData_ContentResource/Activity_HUD_CurrentScore_Notice");
                c.set(c.field(stamp,0x13be0d51),stamps?std::array<float,2>{56.f,56.f}:std::array<float,2>{150.f,56.f});
                const auto label=c.field(stamp,0x94703efc);
                plain_label(c,label,stamps?letter:"");
                label_styles(c,label,"TextStylesList/H52-XBold_White","TextStylesList/H52-XBold_White");
                c.set(c.field(label,0x87ad624c),2);
                c.set(c.field(label,0x3d8639d0),2);
                c.set(c.field(label,0xc13ba637),false);
            }
            if(!hud.background.handle) {
                hud.background=make(c,content_tile_schema);
                c.set(c.field(hud.background,0xa04998ca),false);
                c.set(c.field(hud.background,0xbc750e6a),false);
                c.set(c.path(hud.background,{0x0fb0d794,0x8cc042ef}),
                      style_or(c,"ButtonStyleList/TileButton.RoughPartial.NoDefaultBG","ButtonStyleList/TileButton.RoughPartial.Default"));
            }
            c.set(c.field(hud.background,widget),Widget{asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),{0,hud.row.handle}});
        }
        if(!hud_list_has_items(c,hud.row,shown+1)) {
            std::vector<Widget> letters;
            letters.push_back({asset(c,"UI/Foundations/Components/Text/Label/Widget/Label_Widget"),{0,hud.name.handle}});
            for(unsigned i=0;i<shown;++i)
                letters.push_back({asset(c,"UI/Foundations/Components/Notices/SmallNotice_Widget"),{0,hud.letters[i].handle}});
            // A score row: a fixed name column, so a long name never runs under the score.
            hud_list(c,hud.row,letters,true,stamps?56.f:150.f,10.f,0.f,520.f,stamps?300.f:340.f,stamps);
        }
        // The H52 face fits about 13 characters in the score row's name column.
        plain_label(c,hud.name,stamps || player.name.size()<=13?player.name:player.name.substr(0,12)+".");
        if(!stamps)plain_label(c,c.field(hud.letters[0],0x94703efc),player.value);
        hud.out=player.out;
        if(hud.out) label_styles(c,hud.name,"TextStylesList/H52-XBold_Grey500","TextStylesList/H52-XBold_Grey500");
        else shadow_label(c,hud.name,"TextStylesList/H52-XBold_White");
        if(hud.out) {
            // Use the game's own randomized Rime scribble across the complete
            // row, including the player's name and its stamps.
            mount_hud(c,{},asset(c,"UI/ReSkate/OneUp/EliminatedLine_Widget"),hud.strike_anchor,hud.strike_item,
                hud_strike_id+static_cast<int>(ids.size()),false);
            place(hud.strike_anchor,520.f,56.f,.95f,.82f,1.f,1.f);
            const auto layout=c.field(hud.strike_anchor,0x185a5736);
            const auto offset=.85f*(-height+140.f+static_cast<float>(ids.size())*64.f+56.f);
            c.set(c.field(layout,0x40bfbff4),offset);c.set(c.field(layout,0x1d233266),offset);
        }
        for(unsigned letter=0;letter<shown;++letter) {
            // 1-Up lights each earned penalty; a game mode lights whoever is up (it, their turn) and the local player.
            const bool lit=stamps?earned_one_up_letter(player.penalties,letter):(player.focus || player.self);
            const auto style=lit?record_ref(c,"NoticeStylesList/SmallNotice_ControlSelection_Focus"):
                stamps?record_ref(c,"NoticeStylesList/SmallNotice_ControlSelection_Default"):
                style_or(c,"NoticeStylesList/SmallNotice_Rect_Alpha60_Black_Players","NoticeStylesList/SmallNotice_ControlSelection_Default");
            c.set(c.field(hud.letters[letter],0x8cc042ef),style);
            c.set(c.field(hud.letters[letter],0xa1e84eb1),style);
        }
        ids.push_back(player.id);
        rows.push_back({asset(c,"UI/Foundations/Components/Buttons/ContentPresenterTile/ContentPresenterTile"),{0,hud.background.handle}});
    }
    if(ids!=s.hud_player_ids || !hud_list_has_items(c,s.hud_rows,static_cast<unsigned>(rows.size()))) {
        hud_list(c,s.hud_rows,rows,false,56.f,8.f,0.f,520.f);s.hud_player_ids=std::move(ids);
    }
    publish_hud_items(c,true,s.hud_countdown_visible,s.hud_intro_visible);
    if(!s.hud_ready.exchange(true))logging::log(logging::Level::info,logging::Channel::ui,"Native HUD: {} counters, clock and player rows mounted.",f.title);
}
void native_results_hud(const Context& c, const Feed& f) {
    auto& s=state();
    if(s.results_match!=f.key || f.phase==Feed::Phase::lobby) {
        s.results_match=f.key;s.results_turn=0;s.eliminated_at.clear();
    }
    std::uint32_t turn{};
    if(f.one_up) {
        const auto v=one_up::view();turn=v.state.turn;
        for(const auto& player:v.state.players)
            if(!player.eligible())s.eliminated_at.try_emplace(player.id,v.state.turn);
    }
    const bool finished=f.match && ((f.phase==Feed::Phase::ended && !f.cancelled) || f.practice_done);
    s.hud_results_visible=false;
    if(!finished || !s.hud_ended_at || GetTickCount64()-s.hud_ended_at<f.results_after) return;
    // Built once per finished match: the board never rebuilds while it is shown.
    if(!s.hud_results.handle || s.results_turn!=turn+1) {
        if(!s.hud_results_title.handle)s.hud_results_title=record_model(c,"TD_Hud_ContentResources/TD_HUD_Header_Label");
        plain_label(c,s.hud_results_title,f.title+" RESULTS");
        label_styles(c,s.hud_results_title,"TextStylesList/D94-Caps-Bold_White","TextStylesList/D94-Caps-Bold_White");
        if(!s.hud_results_notice.handle)s.hud_results_notice=record_model(c,"TD_Page_ContentResources/TD_Notice_MenuDescriptionTitle",notice_schema);
        plain_label(c,c.field(s.hud_results_notice,0x94703efc),f.results_notice);
        label_styles(c,c.field(s.hud_results_notice,0x94703efc),"TextStylesList/H64-XBold_White","TextStylesList/H64-XBold_White");
        c.set(c.field(s.hud_results_notice,0x13be0d51),std::array<float,2>{1200.f,130.f});
        auto players=f.rows;
        if(f.one_up) {
            const auto winner=f.winner;
            std::stable_sort(players.begin(),players.end(),[&](const auto& a,const auto& b){
                if(a.id==winner || b.id==winner)return a.id==winner && b.id!=winner;
                const auto at=s.eliminated_at.find(a.id),bt=s.eliminated_at.find(b.id);
                return (at==s.eliminated_at.end()?UINT32_MAX:at->second)>(bt==s.eliminated_at.end()?UINT32_MAX:bt->second);
            });
        }
        if(!s.hud_results_rows.handle)
            s.hud_results_rows=record_model(c,"TD_Scoreboard_ContentResources/LEGACY_TD_Scoreboard");
        // Reuse the actual Throwdown results board and its rank/profile rows.
        // Only the content is changed; its column layout and player styles are
        // supplied by the original LEGACY_Scoreboard_Widget blueprint.
        c.set(c.field(s.hud_results_rows,0x5a7cec85),true);
        c.set(c.field(s.hud_results_rows,0xb7e92e95),true);
        plain_label(c,c.element(c.field(s.hud_results_rows,0x3d98d397),2),f.title);
        std::vector<Ref> rows;unsigned rank{};
        for(const auto& player:players) {
            const bool won=f.winner && player.id==f.winner;
            auto& row=s.result_players[player.id];
            // The profile's name is label 0x042ddc73 inside its 0xaed127c3 model (type 0x4771a9c1,
            // read in game 2026-10-10). The winner gets the gold winner row; if its record ever
            // differs, the stock player row.
            const auto build=[&](bool winner_row) {
                row={};
                row.winner=winner_row;
                row.row=record_model(c,winner_row?"TD_Scoreboard_ContentResources/TD_WinnerProfileInline":"TD_Scoreboard_ContentResources/TD_PlayerProfileInline");
                row.rank=record_model(c,winner_row?"TD_Scoreboard_ContentResources/TD_WinnerRank":"TD_Scoreboard_ContentResources/TD_DefaultRank");
                row.outcome=record_model(c,"TD_Hud_ContentResources/TD_HUD_Header_Label");
                auto rank_widget=read<Widget>(c.address(c.field(row.row,0xc76d8b73)));
                rank_widget.data={0,row.rank.handle};
                c.set(c.field(row.row,0xc76d8b73),rank_widget);
                const auto scores=c.field(row.row,0xa3d2bba9);
                c.array(c.field(scores,0xf2c90867),std::vector<Handle>{});
                c.array(c.field(scores,rows_field),std::vector<Ref>{{0,row.outcome.handle}});
            };
            const auto fill=[&] {
                c.set(c.field(row.row,0x8810a01e),player.self || player.id==f.local);
                plain_label(c,c.path(row.row,{0xaed127c3,0x042ddc73}),player.name);
                plain_label(c,c.field(row.rank,0x2718c842),std::to_string(rank+1));
                plain_label(c,row.outcome,won?(f.one_up?std::string("WINNER"):player.value):f.one_up?(player.connected?"1 U P":"LEFT"):player.value);
            };
            if(!row.row.handle || row.winner!=won) {
                try { build(won); fill(); }
                catch(const std::exception& e) {
                    if(!won) throw;
                    logging::log(logging::Level::warning,logging::Channel::ui,"Native results: winner row unavailable ({}); stock row used.",e.what());
                    build(false);
                }
            }
            fill();
            ++rank;
            rows.push_back({0,row.row.handle});
        }
        const auto player_list=c.field(s.hud_results_rows,0xfcdd0d32);
        c.array(c.field(player_list,0xf2c90867),std::vector<Handle>{});
        c.array(c.field(player_list,rows_field),rows);
        hud_list(c,s.hud_results,{
            {asset(c,"UI/Foundations/Components/Text/Label/Widget/Label_Widget"),{0,s.hud_results_title.handle}},
            {asset(c,"UI/Foundations/Components/Notices/BasicNotice_Widget"),{0,s.hud_results_notice.handle}},
            {asset(c,"UI/Features/CommunityEvent/LEGACY_Scoreboard_Widget"),{0,s.hud_results_rows.handle}}},false,144.f,16.f,140.f*static_cast<float>(players.size())+100.f,1200.f);
        s.results_turn=turn+1;
        logging::log(logging::Level::info,logging::Channel::ui,"Native HUD: {} results leaderboard mounted for {} skater(s).",f.title,players.size());
    }
    mount_hud(c,s.hud_results,asset(c,"UI/Foundations/Components/Lists/LinearList/Widget/LinearList_Widget"),s.hud_results_anchor,s.hud_results_item,hud_results_id,true);
    c.set(c.field(s.hud_results_anchor,0x1cff7243),std::array<float,2>{1200.f,920.f});
    c.set(c.path(s.hud_results_anchor,{0x185a5736,0x99bbed5a}),.5f);
    c.set(c.path(s.hud_results_anchor,{0x185a5736,0x49943bed}),.5f);
    publish_hud_items(c,false,false,false,true);
    s.hud_results_visible=true;
}
void gameplay_hud(const Context& c,std::uint64_t now) {
    auto& s=state();
    try {waiting_hud(c);}
    catch(const std::exception& e) {
        static std::uint64_t report{};
        if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"1-Up native waiting HUD: {}",e.what());}
    }
    Feed f;
    try {f=current_feed();} catch(...) {}
    if(f.match) {
        // The widgets were built for the other kind (1-Up stamps, a game mode's scores): built again.
        const int kind=f.one_up?1:0;
        if(s.hud_kind>=0 && s.hud_kind!=kind) {
            try {publish_hud_items(c,false,false);}catch(...) {}
            clear_hud_state();
        }
        s.hud_kind=kind;
    }
    try {native_countdown_hud(c,f);}
    catch(const std::exception& e) {
        s.hud_countdown_visible=false;s.hud_intro_visible=false;
        try {publish_hud_items(c,s.hud_ready.load(),false);}catch(...) {}
        static std::uint64_t report{};
        if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"Native countdown HUD: {}",e.what());}
    }
    try {native_score_hud(c,f);}
    catch(const std::exception& e) {
        s.hud_ready.store(false);
        try {publish_hud_items(c,false,s.hud_countdown_visible,s.hud_intro_visible);}catch(...) {}
        static std::uint64_t report{};
        if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"Native gameplay HUD: {}",e.what());}
    }
    // The native results board (LEGACY_TD_Scoreboard) crashed the game in its own widget code a
    // moment after it first showed (Skate.exe+0x19126f0, 2026-10-10): it stays off. The score block
    // names the winner, and the game modes' own results screen shows the standings.
    if(native_results_board) try {native_results_hud(c,f);}
    catch(const std::exception& e) {
        s.hud_results_visible=false;
        static std::uint64_t report{};
        if(now>=report) {report=now+10000;logging::log(logging::Level::warning,logging::Channel::ui,"Native results: {}",e.what());}
    }
    // Our own drawn HUD (modes_hud_overlay.cpp) leaves out what the game's widgets now show.
    const bool modes=f.match && !f.one_up;
    overlay::set_native_modes_hud({modes && s.hud_ready.load(),modes && (s.hud_countdown_visible || s.hud_intro_visible),
                                   modes && s.hud_results_visible});
}
void render(const Context& c) {
    auto& s=state(); const auto v=one_up::view(); std::vector<std::string> shown{"rules"};
    const auto note=[&](const char* id,std::string text){row(c,shown,id,text);};
    const auto action=[&](const char* id,std::string text,std::string command,std::string argument={}){row(c,shown,id,text,std::move(command),std::move(argument));};
    footer_action(c,s.footer_back,"BACK","back");
    if(!v.state.match) {
        setting_row(c,shown,"session-type","Session Type","1-Up");
        setting_row(c,shown,"players-setting","Players","",true);
        setting_row(c,shown,"turn-setting","Turn","",true);
        penalty_row(c,shown);
        footer_action(c,s.footer_confirm,s.place_until?"CONNECTING...":"READY UP","place");
        for(const auto& offer:v.offers) action(("offer-"+std::to_string(offer.leader)).c_str(),std::string(offer.started?"Watch ":"Join ")+offer.name+"'s 1-Up","oneup","join "+std::to_string(offer.leader)+" "+std::to_string(offer.match));
    } else {
        if(v.state.phase==one_up::Phase::lobby) {
            const auto ready_count=std::count_if(v.state.players.begin(),v.state.players.end(),[](const auto& p){return p.ready && p.connected;});
            setting_row(c,shown,"session-type","Session Type","1-Up");
            setting_row(c,shown,"lobby-players","Players",std::to_string(v.state.players.size())+" / "+std::to_string(v.state.config.max_players));
            setting_row(c,shown,"lobby-duration","Duration",std::to_string(v.state.config.turn_ms/1000)+" sec");
            penalty_row(c,shown);
            const auto me=std::find_if(v.state.players.begin(),v.state.players.end(),[&](const auto& p){return p.id==v.local;});
            const bool ready=me!=v.state.players.end() && me->ready;
            if(v.state.leader==v.local && ready) footer_action(c,s.footer_confirm,"START 1-UP","oneup","start");
            else if(v.participant) footer_action(c,s.footer_confirm,ready?"READY":"READY UP","oneup","ready");
            else footer_action(c,s.footer_confirm,"JOIN","oneup","enroll");
            note("lobby-help",ready_count<2?"Waiting for skaters to ready up.":"Everyone is ready!");
        }
        else if(v.state.phase==one_up::Phase::finished) {
            note("winner",v.name(v.state.winner)+" WINS!");
            if(v.state.leader==v.local) footer_action(c,s.footer_confirm,"REMATCH","oneup","rematch");
            else footer_action(c,s.footer_confirm,"EXIT 1-UP","oneup","leave");
        } else {
            setting_row(c,shown,"score-setting","Target",std::to_string(static_cast<std::uint32_t>(v.state.target)));
            setting_row(c,shown,"best-setting","Total",std::to_string(static_cast<std::uint32_t>(v.state.best)));
            if(v.state.active) setting_row(c,shown,"active-setting","Up Next",v.name(v.state.active));
            note("notice",v.state.notice);
            footer_action(c,s.footer_confirm,"EXIT 1-UP","oneup","leave");
        }
        footer_action(c,s.footer_back,"EXIT","oneup","leave");
    }
    if(!v.status.empty() && v.status!="You left 1-Up." && !v.status.starts_with("Spot placed.") && !v.status.starts_with("Flag placed.")) note("status",v.status);
    if(!v.state.match && !one_up::flag_placement_status().empty()) note("placement-status",one_up::flag_placement_status());
    if(!s.setup_status.empty() && !v.state.match) note("setup-status",s.setup_status);
    // A primary-action event is consumed by the mounted button handler. Keep
    // the source model current too so a page reload gets the same Confirm.
    const auto mounted=read<Address>(c.address(c.field(s.footer_list,0xf2c90867)));
    const auto mounted_count=mounted?read<std::uint32_t>(mounted-4)&0x7fffffff:0;
    if(mounted_count!=2) {
        c.array(c.field(s.footer_list,rows_field),std::vector<Ref>{});
        c.array(c.field(s.footer_list,0xf2c90867),std::vector<Handle>{s.footer_back.handle,s.footer_confirm.handle});
    }
    const auto current=read<Address>(c.address(c.field(s.details_list,0xf2c90867)));
    const auto count=current?read<std::uint32_t>(current-4)&0x7fffffff:0;
    if(shown!=s.displayed || count!=shown.size()) {
        const auto old_focus=read<int>(c.address(c.field(s.details_list,0x55511280)));
        const auto focused=old_focus>=0 && static_cast<std::size_t>(old_focus)<s.displayed.size()?s.displayed[old_focus]:std::string{};
        std::vector<Handle> handles; int first=-1, preserved=-1;
        for(const auto& id:shown) {
            const auto& r=s.rows.at(id);
            if(r.callback) { if(first<0)first=static_cast<int>(handles.size()); if(id==focused)preserved=static_cast<int>(handles.size()); }
            handles.push_back(r.anchor.handle);
        }
        // These are independent collections: publishing both displays every
        // row twice. Reconcile handles after native mounting clears the list.
        c.array(c.field(s.details_list,0xf2c90867),handles);
        c.set(c.field(s.details_list,0x55511280),preserved>=0?preserved:first);
        s.displayed=std::move(shown);
    }
}
} // namespace
bool open_native_one_up_setup() noexcept {
    auto& s=state();
    try {
        if(!one_up::arm_native_setup(20,s.entry_players.load()))return false;
        s.reset_setup.store(true);
        std::lock_guard lock(s.mutex);
        if(!s.owner.load())s.owner.store(s.manager?s.manager:1);
        s.pending.push_back({s.owner.load(),"open",{},s.pass});
        return true;
    } catch(...) { return false; }
}
bool native_one_up_setup_open() noexcept { return state().setup_open.load(std::memory_order_acquire); }
std::uint64_t native_one_up_card() noexcept { return state().card.handle; }
bool native_one_up_hud_ready() noexcept { return state().hud_ready.load(std::memory_order_acquire); }
bool release_native_one_up_menu(std::uintptr_t base) noexcept {
    auto& s=state(); if(s.owned.empty()) { s.owner.store(0); s.setup_open.store(false); return true; }
    try { const auto ui=read<Address>(base+build::engine::ui_manager); const auto manager=ui?read<Address>(ui+0x140):0;
        require(manager!=0,"1-Up UI manager unavailable during cleanup.");
        if(manager!=s.manager) { s.owned.manager_replaced(); s.marker_originals.clear(); clear_hud_state(); s.official_originals.clear(); s.official_panels.clear(); s.official_headers.clear(); s.official_contents.clear(); s.official_slot_panels.clear(); s.official_sliders.clear(); s.solo_sliders.clear(); s.waiting_originals.clear(); s.waiting_widgets.clear(); s.waiting_backups.clear(); s.waiting_presentations.clear(); s.waiting_counts.clear(); s.waiting_label={}; s.waiting_countdown={}; s.waiting_badge={}; s.waiting_diagnostic=0; s.owner.store(0); s.setup_open.store(false); s.page={}; s.cards={}; s.panel={}; s.badge={}; s.duration_selector={}; s.originals.clear(); s.rows.clear(); s.assets.clear(); s.displayed.clear(); s.generations.clear(); s.details=false; return true; }
        const Context c(base,manager); game::ModelWriteLock lock(manager); release(c); s.generations.clear(); return true;
    } catch(const std::exception& e) { logging::log(logging::Level::warning,logging::Channel::ui,"1-Up native menu cleanup: {}",e.what()); return false; }
}
void tick_native_one_up_menu(std::uintptr_t base,bool loading) noexcept {
    auto& s=state(); const auto now=GetTickCount64(); if(loading || (now<s.next_tick && !one_up::flag_placement_active())) return; s.next_tick=now+200;
    // Game mode commands from a flag's waiting card (and its native Start/Leave), run once the
    // model lock is released.
    std::vector<std::string> mode_commands;
    {
        const auto flag=one_up::take_mode_flag_requests();
        if(flag.start) mode_commands.push_back("start");
        if(flag.stop) mode_commands.push_back(modes::menu_view().leading?"stop":"leave");
    }
    const auto run_mode_commands=[&] {
        for(const auto& text:mode_commands) {
            if(text.empty()) continue;
            std::vector<std::string> words;
            for(std::size_t at=0;at<text.size();) {
                auto end=text.find(' ',at); if(end==std::string::npos) end=text.size();
                if(end>at) words.push_back(text.substr(at,end-at));
                at=end+1;
            }
            const auto verb=words.front(); words.erase(words.begin());
            try {
                const auto said=modes::command(verb,words);
                logging::log(logging::Level::info,logging::Channel::ui,"Throwdown flag: mode {} -> {}",text,said);
            } catch(...) {}
        }
    };
    struct RunAfter { const decltype(run_mode_commands)& run; ~RunAfter(){ run(); } } run_after{run_mode_commands};
    try {
        const auto ui=read<Address>(base+build::engine::ui_manager); const auto manager=ui?read<Address>(ui+0x140):0; if(!manager)return;
        const Context c(base,manager); game::ModelWriteLock lock(manager);
        retire_solo_pending_queue(c);
        one_up::tick_flag_placement(c);
        ++s.pass;
        if(s.manager && s.manager!=manager) { s.owned.manager_replaced(); s.marker_originals.clear(); clear_hud_state(); s.official_originals.clear(); s.official_panels.clear(); s.official_headers.clear(); s.official_contents.clear(); s.official_slot_panels.clear(); s.official_sliders.clear(); s.solo_sliders.clear(); s.waiting_originals.clear(); s.waiting_widgets.clear(); s.waiting_backups.clear(); s.waiting_presentations.clear(); s.waiting_counts.clear(); s.waiting_label={}; s.waiting_countdown={}; s.waiting_badge={}; s.waiting_diagnostic=0; s.owner.store(0); s.setup_open.store(false); s.page={}; s.cards={}; s.panel={}; s.badge={}; s.duration_selector={}; s.originals.clear(); s.rows.clear(); s.assets.clear(); s.displayed.clear(); s.generations.clear(); s.details=false; }
        s.manager=manager; s.base=base;
        session_marker_controls(c);
        if(s.page.handle && c.type_of(s.page.handle)!=s.page.type) release(c);
        // The game creates new page/list generations when Throwdowns reopens.
        // Find fresh instances rather than leaving the card on a retired page.
        // The 1-UP card is the game modes grid's (modes_card.cpp, open_native_one_up_setup): this
        // adapter no longer adds its own card to the Throwdowns row (the grid holding this one's
        // card crashed the game, 2026-10-10; a longer row overflowed this adapter's list).
        if(own_card) for(const auto& p:c.roots({page.hash})) if(const auto list=card_list(c,p.model)) {
            if(s.page.handle && (p.model.handle!=s.page.handle || list->handle!=s.cards.handle) && !s.generations.contains({p.model.handle,list->handle})) release(c);
            if(!s.page.handle && !s.generations.contains({p.model.handle,list->handle})) { initialize(c,p.model,*list); break; }
        }
        if(!s.page.handle) {
            // The native page is retired when skating resumes. Gameplay HUDs
            // must keep updating without a Throwdowns menu generation.
            if(!s.owner.load())s.owner.store(manager);
            // Native navigation retires the card page before setup mounts.
            // Continue adapting the independent native panel in that interval.
            official_setup(c);
            stock_solo_controls(c);
            std::vector<Action> pending;
            {std::lock_guard pending_lock(s.mutex);pending=std::exchange(s.pending,{});}
            for(const auto& a:pending)if(a.generation==s.owner.load()) {
                // The grid's 1-UP card armed the native setup: host a session for others to join.
                if(a.command=="open") { if(one_up::native_setup_active() && s.max_players!=1 && !model().active)queue_command("host","",{}); }
                else if(a.command=="oneup")one_up::queue(a.argument);
                else if(a.command=="mode")mode_commands.push_back(a.argument);
            }
            gameplay_hud(c,now);
            return;
        }
        if(one_up::flag_lobby_requested() && one_up::view().state.match) {
            one_up::flag_lobby_shown(); // Keep the native waiting-for-skaters HUD.
        }
        publish_texture(c, std::array{c.field(s.category,0x189084da),c.field(s.category,0xc2917efc)}, "UI/ReSkate/OneUp/img_OneUp_Blue_1024");
        if(s.badge.handle) publish_texture(c,std::array{c.field(s.badge,0x6e469d2a)},"UI/ReSkate/OneUp/img_OneUp_Blue_1024");
        const auto resize=[&](Value t,Value cat,Value desc,const Original& original) {
            auto dimensions=original.tile_size; dimensions[0]*=.74f;
            if(read<std::array<float,2>>(c.address(c.field(t,0x868043e1)))!=dimensions) c.set(c.field(t,0x868043e1),dimensions);
            auto ds=original.description_size; ds[0]*=.74f;
            if(read<std::array<float,2>>(c.address(c.field(desc,0x1cff7243)))!=ds) c.set(c.field(desc,0x1cff7243),ds);
            for(const auto [hash,value]:std::array{std::pair{0x495ffd43U,original.icon_width*.74f},std::pair{0x24fb5ca8U,original.icon_height*.74f}})
                if(read<float>(c.address(c.field(cat,hash)))!=value) c.set(c.field(cat,hash),value);
        };
        // The game modes cards (modes_card.cpp) size every card on the page, this one too.
        if(!native_modes_card_active()) {
            for(const auto& original:s.originals) resize(original.tile,original.category,original.description,original);
            resize(s.card,s.category,s.description,s.originals[1]);
        }
        std::vector<Action> pending; {std::lock_guard pending_lock(s.mutex); pending=std::exchange(s.pending,{});}
        // Keep the card's callback reserved even while its setup is displayed.
        callback(c,"open");
        unsigned card_count{},card_stride{};
        const auto cards=c.array(c.field(s.cards,rows_field),8,card_count,card_stride);
        require(card_stride==sizeof(Ref),"1-Up card list changed.");
        std::vector<Ref> refs(card_count); if(!cards.empty())std::memcpy(refs.data(),cards.data(),cards.size());
        if(std::none_of(refs.begin(),refs.end(),[&](const auto& ref){return ref.handle==s.card.handle;})) {
            require(refs.size()==3,"1-Up card list was replaced.");
            refs.push_back({0,s.card.handle}); c.array(c.field(s.cards,rows_field),refs);
        }
        for(const auto& a:pending) if(a.generation==s.owner.load()) {
            logging::log(logging::Level::debug,logging::Channel::ui,"1-Up native menu action: {}.",a.command);
            if(a.command=="open") {
                // Navigation already ran through the native button. Hosting
                // must not enqueue a second navigation after setup opens.
                if(one_up::native_setup_active() && s.max_players!=1 && !model().active)queue_command("host","",{});
            }
            else if(a.command=="back") switch_page(c,false);
            else if(a.command=="duration") s.seconds=s.seconds>=120?10:s.seconds+10;
            else if(a.command=="players") s.max_players=s.max_players>=6?1:s.max_players+1;
            else if(a.command=="place" && !s.place_until) {
                const auto v=one_up::view();
                if(v.can_create) {
                    if(one_up::begin_flag_placement(s.seconds,s.max_players)) { s.setup_status.clear(); switch_page(c,false); }
                }
                else {
                    s.place_seconds=s.seconds; s.place_players=s.max_players; s.place_until=now+30000;
                    s.setup_status="Connecting multiplayer, then opening flag placement...";
                    if(!model().active && !queue_command("host","",{})) {
                        s.place_until=0; s.setup_status="Could not start multiplayer. Try Confirm again.";
                    }
                }
            }
            else if(a.command=="oneup") one_up::queue(a.argument);
            else if(a.command=="mode") mode_commands.push_back(a.argument);
        }
        if(s.place_until) {
            const auto v=one_up::view();
            if(v.can_create) {
                s.place_until=0; s.setup_status.clear();
                one_up::begin_flag_placement(s.place_seconds,s.place_players,true);
            } else if(now>=s.place_until) {
                s.place_until=0;
                s.setup_status="Could not place 1-Up. Load the map, finish your current Throwdown, and try Confirm again. "+model().status;
            }
        }
        try { official_setup(c); }
        catch(const std::exception& e) {
            static std::uint64_t setup_retry_log{};
            if(now>=setup_retry_log) {
                setup_retry_log=now+10000;
                logging::log(logging::Level::warning,logging::Channel::ui,"1-Up official setup: {}",e.what());
            }
        }
        s.entry_seconds.store(s.seconds);s.entry_players.store(s.max_players);
        stock_solo_controls(c);
        gameplay_hud(c,now);
    } catch(const std::exception& e) {
        logging::log(logging::Level::warning,logging::Channel::ui,"1-Up native menu: {}",e.what());
        s.next_tick=now+5000; release_native_one_up_menu(base);
    }
}
} // namespace dingosdk::multiplayer

