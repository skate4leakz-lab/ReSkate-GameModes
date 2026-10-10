#include "Extension/UI/NativeMenu/native_menu_lifetime.h"
#include "Extension/UI/NativeMenu/native_menu_ownership.h"
#include "Extension/UI/NativeMenu/native_hud_selection.h"
#include "Extension/UI/NativeMenu/native_hud_values.h"
#include <cstdio>
#include <array>
#include <map>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <functional>

using dingosdk::multiplayer::menu_data::OwnedMenuModels;
using dingosdk::multiplayer::menu_data::MenuLifetime;
using dingosdk::multiplayer::menu_data::MenuAction;
using dingosdk::multiplayer::menu_data::action_slot;
struct Model { unsigned handle, type; };
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void map_transition() {
    OwnedMenuModels<Model> owned;
    std::map<unsigned, unsigned> live{{1, 100}, {2, 200}, {3, 300}};
    owned.track({2, 200}); // First pause-menu instance.
    owned.track({3, 300}); // Partially built replacement instance.
    owned.track({3, 300}); // Engine create can return an existing owned root.
    bool attached = true, assets_loaded = true;
    std::vector<unsigned> destroyed;
    const auto destroy = [&](Model value) {
        check(!attached, "Detach tabs before destroying their data");
        check(assets_loaded, "Release list templates while their assets are still loaded");
        check(live.at(value.handle) == value.type, "Destroy only matching owned roots");
        live.erase(value.handle);
        destroyed.push_back(value.handle);
    };
    owned.release([&] { attached = false; }, destroy);
    assets_loaded = false; // The common loader may now submit either menu's request.
    check(owned.empty() && live.size() == 1 && live.contains(1), "Native base-game roots remain owned by the game");
    check(destroyed == std::vector<unsigned>{3, 2}, "Partial builds and older menu models are released once");
    owned.release([&] { throw std::runtime_error("Repeated teardown must be inert"); }, destroy);
    assets_loaded = true;
    live.emplace(4, 400);
    owned.track({4, 400}); // Reopening after loading creates a fresh menu.
    owned.release([] {}, destroy);
    check(destroyed.back() == 4 && owned.empty(), "The next world gets an independent menu lifetime");
}
void interrupted_cleanup() {
    OwnedMenuModels<Model> owned;
    owned.track({10, 100}); owned.track({20, 200});
    unsigned calls{};
    bool failed{};
    try {
        owned.release([] { throw std::runtime_error("Detach failed"); }, [&](Model) { ++calls; });
    } catch (const std::runtime_error&) { failed = true; }
    check(failed && calls == 0 && !owned.empty(), "Failed detach must preserve models and prevent unload");
    std::vector<unsigned> destroyed;
    try {
        owned.release([] {}, [&](Model value) {
            if (value.handle == 10) throw std::runtime_error("Destroy failed");
            destroyed.push_back(value.handle);
        });
    } catch (const std::runtime_error&) { }
    owned.release([] {}, [&](Model value) { destroyed.push_back(value.handle); });
    check(destroyed == std::vector<unsigned>{20, 10}, "Retry must finish cleanup without double destruction");
    owned.track({30, 300});
    owned.manager_replaced();
    owned.release([] {}, [&](Model) { throw std::runtime_error("Old manager handle reused"); });
    check(owned.empty(), "Replacing the native manager must discard its expired handles");
}
void window_close() {
    MenuLifetime lifetime;
    lifetime.before_transition(13, [] {});
    std::array<OwnedMenuModels<Model>, 2> pages;
    pages[0].track({10, 100}); pages[1].track({20, 200});
    std::array<bool, 2> attached{true, true};
    bool assets_loaded = true;
    unsigned destroyed{}, cleanups{};
    const auto cleanup = [&] {
        ++cleanups;
        check(lifetime.blocked(), "Stop updates before cleanup can invoke UI callbacks");
        for (unsigned slot = 0; slot < pages.size(); ++slot) {
            pages[slot].release([&] { attached[slot] = false; }, [&](Model) {
                check(!attached[slot] && assets_loaded,
                    "Close-window cleanup must release widget references before asset unload");
                ++destroyed;
            });
        }
    };
    for (unsigned state : {13U, 13U, 21U}) lifetime.before_transition(state, cleanup);
    check(cleanups == 0 && !lifetime.blocked(), "Active-state notifications must not destroy menu models");
    lifetime.before_transition(24, cleanup); // Before native state-24 handler.
    assets_loaded = false; // The native handler unloads the widget asset arena.
    for (unsigned state : {24U, 25U, 25U, 13U, 21U, 14U}) {
        lifetime.before_transition(state, cleanup);
        if (!lifetime.blocked()) pages[0].track({30, 300}); // A remaining client tick.
    }
    check(cleanups == 1 && destroyed == 2 && pages[0].empty() && pages[1].empty(),
        "Both menu tabs must stay released through all remaining shutdown ticks");
    for (auto& page : pages)
        page.release([] {}, [&](Model) { throw std::runtime_error("Late widget release reproduces the shutdown crash"); });

    MenuLifetime failed;
    failed.before_transition(24, [] {}); // Cleanup can report failure without clearing the latch.
    check(failed.blocked(), "Even an incomplete cleanup must prevent new models during shutdown");
}
void native_transition_without_load_request() {
    // The multiplayer crash used the game's transition path. No ReSkate load
    // was queued, so the scheduler's explicit cleanup never ran. Reopening the
    // pause menu and later ticks must not retain/recreate the old asset refs.
    for (unsigned leaving : {14U, 22U, 3U}) {
        MenuLifetime lifetime;
        std::array<OwnedMenuModels<Model>, 2> pages;
        check(lifetime.blocked(), "Do not build menus before the first active world");
        lifetime.before_transition(13, [] { throw std::runtime_error("Unexpected initial cleanup"); });
        pages[0].track({10, 100}); pages[1].track({20, 200});
        bool assets_loaded = true;
        unsigned cleanups{}, destroyed{}, published{}, actions{};
        const auto cleanup = [&] {
            ++cleanups;
            check(lifetime.blocked(), "Block reentrant callbacks before native transition cleanup");
            for (auto& page : pages) page.release([] {}, [&](Model) {
                check(assets_loaded, "Destroy menu asset references before the engine frees assets");
                ++destroyed;
                if (!lifetime.blocked()) ++actions;
            });
        };
        lifetime.before_transition(leaving, cleanup);
        assets_loaded = false;
        for (unsigned state : {leaving, 3U, 4U, 8U, 9U, 12U}) {
            lifetime.before_transition(state, cleanup);
            if (!lifetime.blocked()) { ++published; ++actions; }
        }
        check(cleanups == 1 && destroyed == 2 && published == 0 && actions == 0,
            "Native unload must clean both pages once and suppress updates/actions throughout loading");
        for (auto& page : pages) page.release([] {}, [&](Model) {
            throw std::runtime_error("Late model destruction reproduces the multiplayer crash");
        });
        assets_loaded = true;
        lifetime.before_transition(leaving == 22 ? 21 : 13, cleanup);
        check(!lifetime.blocked(), "The next active level/sublevel must allow reopening the menu");
        pages[0].track({30, 300}); pages[1].track({40, 400});
        lifetime.before_transition(14, cleanup);
        check(cleanups == 2 && destroyed == 4, "Clean subsequent worlds, not just the first transition");
        lifetime.before_transition(24, cleanup);
        check(lifetime.blocked() && destroyed == 4, "Shutdown during loading must not release roots twice");
    }
}
void action_slots() {
    // A page's buttons, rendered the way the menu does it: every button asks for
    // its command's slot on every pass, and a map load starts a new generation.
    constexpr std::size_t capacity = 8;
    std::array<MenuAction, capacity> actions;
    std::size_t used{};
    std::uint64_t pass{}, generation{};
    const auto ask = [&](std::string command, std::string argument = {}) {
        const auto slot = action_slot(used, capacity, [&](std::size_t i) -> const MenuAction& { return actions[i]; },
            command, argument, pass);
        if (!slot) return capacity;
        actions[*slot] = {generation, std::move(command), std::move(argument), pass};
        if (*slot == used) ++used;
        return *slot;
    };
    const auto render = [&](std::initializer_list<const char*> buttons) {
        ++pass;
        std::vector<std::size_t> slots;
        for (const auto* button : buttons) slots.push_back(ask("load", button));
        return slots;
    };
    const auto first = render({"a", "b", "c", "d", "e", "f"});
    check(used == 6 && first == std::vector<std::size_t>{0, 1, 2, 3, 4, 5}, "Each command takes the next unused slot");
    // Fifteen map loads filled the table when every generation took new slots.
    for (unsigned load = 0; load < 100; ++load) {
        ++generation;
        check(render({"a", "b", "c", "d", "e", "f"}) == first && used == 6,
            "The same buttons after a map load keep their slots");
        check(actions[0].generation == generation, "A kept slot belongs to the menu that is up now");
    }
    // A list whose rows change (lobbies, players): the slots of rows that went away are reused.
    check(render({"a", "b", "c", "d", "e", "f", "g", "h"}).back() == 7 && used == capacity, "The table can fill");
    check(render({"a", "b", "c", "d", "e", "f", "g", "h"}).back() == 7, "A full table still serves the commands it holds");
    check(ask("load", "i") == capacity, "A slot a button asked for in this pass or the last is not given away");
    render({"a", "b", "c", "d", "e", "f"});
    check(ask("load", "i") == capacity, "One pass without its button is not enough: the pass may not be over");
    const auto later = render({"a", "b", "c", "d", "e", "f", "i"});
    check(later.back() == 6 && actions[6].argument == "i", "A slot no button has asked for is handed to a new command");
    check(render({"a", "b", "c", "d", "e", "f", "i", "g"}).back() == 7 && actions[7].argument == "g",
        "A command that lost its slot gets another when its button returns");
}
void live_hud_binding() {
    using dingosdk::multiplayer::menu_data::HudRootCandidate;
    using dingosdk::multiplayer::menu_data::mounted_hud_root;
    // These identities/order reproduce the captured running Spot Battle HUD:
    // first the registered authored defaults, then the populated live root.
    std::array roots{HudRootCandidate{0x1000103210000ULL,false,0},
                     HudRootCandidate{0x16ced0000ULL,true,3}};
    check(mounted_hud_root(roots)==0x16ced0000ULL,
          "Bind the rendered Spot Battle HUD, not its registered template");
    roots[0].active=true; // Appended 1-Up items must not qualify a template.
    check(mounted_hud_root(roots,roots[0].handle)==roots[1].handle,
          "Ignore an earlier mistaken binding with no game-owned HUD contents");
    std::swap(roots[0],roots[1]);
    check(mounted_hud_root(roots)==0x16ced0000ULL,
          "Registry order cannot decide which HUD is rendered");
    roots[0].active=false;
    check(mounted_hud_root(roots)==0,"Wait when only inactive/empty HUD roots remain");
    roots[1]={0x180000000ULL,true,2};
    check(mounted_hud_root(roots,0x16ced0000ULL)==0x180000000ULL,
          "Rebind when the previous live HUD retires");
}
void native_hud_clock_and_penalties() {
    using dingosdk::multiplayer::menu_data::native_hud_seconds;
    using dingosdk::multiplayer::menu_data::earned_one_up_letter;
    check(native_hud_seconds(3000) == 3 && native_hud_seconds(2980) == 3,
          "The native animated countdown receives whole seconds, rounded up");
    check(native_hud_seconds(1000) == 1 && native_hud_seconds(999) == 1 &&
          native_hud_seconds(1) == 1 && native_hud_seconds(0) == 0,
          "GO begins only after the synchronized clock reaches zero");
    check(native_hud_seconds(28000) == 28,
          "Match the captured native Spot Battle timer's 28-second value");
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    check(native_hud_seconds(maximum) == maximum / 1000 + 1,
          "Rounding large clock values cannot overflow");
    for (unsigned penalties = 0; penalties <= 3; ++penalties) {
        unsigned earned{};
        for (unsigned letter = 0; letter < 3; ++letter)
            earned += earned_one_up_letter(penalties, letter);
        check(earned == penalties, "Show all three stamps and highlight each earned 1-U-P penalty");
        check(!earned_one_up_letter(penalties, 3), "There is no fourth penalty stamp");
    }
}
void native_one_up_slider_values() {
    using namespace dingosdk::multiplayer::menu_data;
    check(native_slider_choice((1.16f-1.f)/5.f,1,6,1)==1,
          "The captured fractional Players value snaps to a whole player");
    check(native_slider_choice((106.8f-10.f)/110.f,10,120,10)==110,
          "The captured fractional timer snaps to a ten-second choice");
    for(unsigned players=1;players<=6;++players)
        check(native_slider_choice(native_slider_position(players,1,6),1,6,1)==players,
              "All supported player counts round trip through native normalized storage");
    for(unsigned seconds=10;seconds<=120;seconds+=10)
        check(native_slider_choice(native_slider_position(seconds,10,120),10,120,10)==seconds,
              "Native turn times round trip without fractional display values");
    check(native_slider_choice(native_slider_position(20,10,120),10,120,10)==20,
          "A fresh 1-Up setup defaults to twenty seconds");
    check(native_slider_choice(std::numeric_limits<float>::quiet_NaN(),1,6,1)==1 &&
          native_slider_choice(4.f,1,6,1)==6 && native_slider_choice(-4.f,1,6,1)==1,
          "Invalid native input cannot create an unsupported player count");
}
void wrapped_waiting_notification() {
    using namespace dingosdk::multiplayer::menu_data;
    // Reproduce the captured live secondary stack: the outer Foundations
    // wrapper points to the actual Spot Battle waiting notification.
    std::map<std::uint64_t,NotificationMountNode> slots{
        {10,{0x174b30000ULL,0xffb83e48,11}},
        {11,{0x174a90000ULL,0x1053b9ec,0}},
        {20,{0x15d8b0000ULL,0x1053b9ec,0}}, // Unmounted RIP-score template.
        {30,{999,0x0e3be640,11}}, // Unrelated HUD content is not a queue wrapper.
        {40,{888,0xffb83e48,41}}, {41,{887,0xffb83e48,40}}};
    const auto read=[&](std::uint64_t handle){return slots.at(handle);};
    check(mounted_notification(10,read)==NotificationMount{11,0x174a90000ULL},
          "Resolve the rendered waiting model and its inner presenter through the native wrapper");
    slots[11]={123,0x608f1aab,0}; // During play, inner slot shows the native Leave button.
    const auto original=[&](std::uint64_t handle) {
        return handle==11?NotificationMountNode{0x174a90000ULL,0x1053b9ec,0}:read(handle);
    };
    check(mounted_notification(10,original)==NotificationMount{11,0x174a90000ULL},
          "A compact Leave presentation retains the same queue identity for restoration");
    check(!mounted_notification(30,read) && !mounted_notification(40,read) &&
          !mounted_notification(0,read),"Ignore unrelated wrappers, missing mounts and cycles");
}
void private_throwdown_graphs() {
    using dingosdk::multiplayer::menu_data::UiCloneCache;
    struct Model {std::string name;std::vector<unsigned> children;};
    // Header/body share a native label; the callback owner points back to the
    // panel. Another stock panel also uses that same label and must stay stock.
    std::map<unsigned,Model> nodes{{1,{"Spot Battle",{2,3}}},{2,{"Spot rules",{4}}},
        {3,{"Footer",{4}}},{4,{"Native label",{1}}},{5,{"Skate Jam",{4}}}};
    UiCloneCache<unsigned,unsigned> one_up;unsigned next=100,created{};
    std::function<unsigned(unsigned)> copy=[&](unsigned source) {
        return one_up.clone(source,[&] {
            const auto id=++next;nodes[id]=nodes.at(source);++created;return id;
        },[&](unsigned owned) {
            const auto original=nodes.at(owned).children;
            std::vector<unsigned> children;for(const auto child:original)children.push_back(copy(child));
            nodes.at(owned).children=std::move(children);
        });
    };
    const auto private_panel=copy(1),private_label=copy(4);
    nodes.at(private_panel).name="1-UP";nodes.at(private_label).name="Three penalties";
    check(created==4 && private_panel!=1 && private_label!=4,"Clone every referenced model once, including a cycle");
    check(nodes.at(1).name=="Spot Battle" && nodes.at(4).name=="Native label" && nodes.at(5).children[0]==4,
        "Changing 1-Up leaves the original Spot Battle and Skate Jam data untouched");
    check(nodes.at(copy(2)).children[0]==private_label && nodes.at(copy(3)).children[0]==private_label &&
          nodes.at(private_label).children[0]==private_panel,"Private presenters share only private mutable identities");
    UiCloneCache<unsigned,unsigned> next_open;
    const auto reopened=next_open.clone(4,[&]{const auto id=++next;nodes[id]=nodes.at(4);return id;},[](unsigned){});
    check(reopened!=private_label && nodes.at(reopened).name=="Native label","A later menu starts from clean stock data");
    using namespace dingosdk::multiplayer::menu_data;
    const auto original_four=native_slider_choice(.25f,2,10,1);
    check(original_four==4 && native_slider_choice(native_slider_position(original_four,1,10),1,10,1)==4,
        "Enabling solo keeps the stock slider's selected player count");
    check(native_slider_choice(0.f,1,10,1)==1,"The stock Players slider can select exactly one skater");
}
int main() {
    try {
        map_transition(); interrupted_cleanup(); window_close(); native_transition_without_load_request(); action_slots(); live_hud_binding(); native_hud_clock_and_penalties(); native_one_up_slider_values(); wrapped_waiting_notification(); private_throwdown_graphs();
        std::puts("Native menu lifetime: scheduled/native transitions, blocked callbacks, reload, shutdown, partial builds, retries and action slots passed.");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
