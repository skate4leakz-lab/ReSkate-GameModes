#include <Windows.h>
static ULONGLONG test_now=1000;
static ULONGLONG placement_test_clock() { return test_now; }
#define GetTickCount64 placement_test_clock
#include "Extension/Throwdowns/one_up_placement.cpp"
#undef GetTickCount64
#include <iostream>
#include <limits>

namespace {
using namespace dingosdk::multiplayer;
one_up::View current_view;
std::vector<one_up::SpawnRequest> handed_off;
std::vector<std::uint32_t> destroyed, navigations;
std::vector<std::string> commands;
unsigned released{};
std::uint32_t nav{};
bool available=true;
}
namespace dingosdk::multiplayer {
void release_one_up_native_placeholder() noexcept { ++released; }
bool prepare_throwdown_injection() noexcept { return true; }
bool queue_throwdown_destroy(std::uint32_t id) noexcept { destroyed.push_back(id); return true; }
namespace one_up {
View view() { return current_view; }
void create_at_spawn(const SpawnRequest& request) { handed_off.push_back(request); }
void queue(std::string_view command) {
    commands.emplace_back(command);
    if(command=="leave")current_view.state={};
}
}
namespace menu_data {
unsigned size(Address) { return 4; }
void require(bool yes,const char* message) { if(!yes) throw std::runtime_error(message); }
std::vector<Root> Context::roots(std::initializer_list<std::uint32_t>) const { return available?std::vector<Root>{{{1,1},0}}:std::vector<Root>{}; }
Value Context::field(Value,std::uint32_t hash) const { return {hash,1}; }
Address Context::address(Value) const { return reinterpret_cast<Address>(&nav); }
void Context::publish(Value,const void* bytes) const { std::memcpy(&nav,bytes,4); navigations.push_back(nav); }
void Context::array(Value,std::span<const std::byte> bytes,unsigned count) const {
    std::int32_t metadata{}; std::memcpy(&metadata,bytes.data(),4);
    require(count==1 && metadata==-1,"Navigation metadata differs from native command.");
}
}
}
int main() {
    using namespace dingosdk::multiplayer::one_up;
    using dingosdk::game::native_name_hash;
    const menu_data::Context context(0,0);
    unsigned count{};
    const auto check=[&](bool value) { ++count; if(!value) throw std::runtime_error("Flag bridge check "+std::to_string(count)); };
    const auto tick=[&] { ++test_now; tick_flag_placement(context); };
    try {
        check(!begin_flag_placement(30,4));
        current_view.can_create=true; current_view.world=11; current_view.local=42;
        check(!begin_flag_placement(30,7));
        check(!begin_flag_placement(30,0));
        check(begin_flag_placement(30,1));abandon_flag_placement();released=0;
        check(begin_flag_placement(40,6) && !begin_flag_placement(30,4));
        available=false;
        try { tick(); check(false); } catch(const std::runtime_error&) {}
        check(flag_placement_active() && navigations.empty());
        available=true; tick(); check(nav==native_name_hash("SpotBattle"));
        observe_flag_created(); observe_flag_graph(0x0f131312);
        tick(); check(navigations.size()==1); // Never overwrite an unconsumed navigation.
        nav=0; tick(); check(nav==native_name_hash("ThrowdownPlacement"));
        observe_flag_position(100,{7,8,9}); check(!placement().spawn); // Not at the flag yet.
        observe_flag_graph(0x67122a51); observe_flag_graph(0x08a3b8c5);
        check(!placement().flag_seen); // S.K.A.T.E. graphs cannot advance Spot Battle ownership.
        observe_flag_graph(0x9700be92);
        check(nav==native_name_hash("ThrowdownPlacement") && placement().flag_seen);
        nav=0; tick();
        observe_flag_position(100,{std::numeric_limits<float>::infinity(),8,9}); check(!placement().spawn);
        const Facing facing{0,0,-1,0,1,0,1,0,0};
        auto flag_transform=spawn_transform({7,8,9},facing);
        auto invalid_transform=flag_transform;invalid_transform[0]=3;
        observe_flag_position(100,{7,8,9},&invalid_transform);check(!placement().spawn);
        observe_flag_position(100,{7,8,9},&flag_transform); observe_flag_exit(false); tick();
        check(destroyed.empty() && handed_off.size()==1);
        check(!flag_placement_active() && flag_registration_owned() && owns_flag_registration(100) && !owns_flag_registration(999) && released==1);
        check(handed_off[0].position==std::array<float,3>{7,8,9} && handed_off[0].seconds==40 && handed_off[0].players==6 && handed_off[0].world==11);
        check(handed_off[0].facing==facing);
        tick(); check(handed_off.size()==1);
        current_view.state.match=77; nav=0; tick(); check(!nav && !flag_lobby_requested()); // Stay skating with native waiting HUD.
        test_now+=300000; tick(); check(flag_registration_owned() && destroyed.empty()); // Waiting has no auto-start deadline.
        current_view.state.match=0;
        tick(); check(destroyed==std::vector<std::uint32_t>{100});
        observe_flag_destroyed(999); test_now+=600; tick(); check(flag_registration_owned());
        observe_flag_destroyed(100); test_now+=501; tick(); check(!flag_registration_owned() && released==2);
        check(begin_flag_placement(30,4)); nav=0; tick(); observe_flag_created(); observe_flag_exit(true); tick();
        check(!flag_placement_active() && released==3 && handed_off.size()==1);
        observe_flag_position(200,{7,8,9}); observe_flag_exit(false); tick(); check(handed_off.size()==1); // Unowned native match.
        check(begin_flag_placement(30,4)); current_view.world=12; tick(); check(!flag_placement_active() && destroyed.size()==1);
        check(begin_flag_placement(30,4)); abandon_flag_placement(); check(!flag_placement_active());
        check(begin_flag_placement(30,4)); nav=0; tick(); observe_flag_created(); observe_flag_graph(0x0f131312);
        nav=0; tick(); check(nav==native_name_hash("ThrowdownPlacement"));
        nav=native_name_hash("Back"); observe_flag_graph(0x9700be92);
        check(nav==native_name_hash("Back")); // Never overwrite another queued user action.
        nav=0; tick(); check(nav==0 && placement().step==Step::placing);
        abandon_flag_placement(); check(!flag_placement_active());
        check(begin_flag_placement(30,4,true));nav=0;tick();
        observe_flag_created();observe_flag_graph(0x0f131312);nav=0;
        test_now+=20000;tick();
        check(native_setup_active() && nav==0); // Original setup remains open until Confirm.
        set_native_setup_options(50,6);set_native_setup_options(30,7);
        check(placement().seconds==50 && placement().players==6);
        nav=native_name_hash("ThrowdownPlacement");observe_flag_graph(0x9700be92);
        check(!native_setup_active() && nav==native_name_hash("ThrowdownPlacement"));
        abandon_flag_placement();
        // Card navigation belongs to the bridge. Ownership is armed before
        // the native setup opens, including when no local capture exists yet.
        current_view.world=0;current_view.local=0;current_view.can_create=false;
        nav=0;const auto before_native=navigations.size();
        check(!arm_native_setup(30,7));
        check(arm_native_setup(30,6));tick();
        check(native_setup_active() && navigations.size()==before_native+1 && nav==native_name_hash("SpotBattle"));
        nav=0;
        observe_flag_created();observe_flag_graph(0x0f131312);
        current_view.world=22;current_view.local=42;tick();
        test_now+=20000;tick();
        check(native_setup_active() && placement().world==22 && placement().players==6 && nav==0);
        nav=native_name_hash("ThrowdownPlacement");observe_flag_graph(0x9700be92);
        check(nav==native_name_hash("ThrowdownPlacement"));
        abandon_flag_placement();nav=0;
        check(arm_native_setup(30,4));
        test_now+=10001;tick();
        check(!flag_registration_owned() && nav==0); // Failed open cannot close another menu.
        check(arm_native_setup(40,5));
        check(arm_native_setup(50,6)); // Reopen after Back is not ignored.
        tick();check(nav==native_name_hash("SpotBattle"));nav=0;
        observe_flag_created();observe_flag_graph(0x0f131312);
        // Native Spot Battle Confirm initializes flag placement directly.
        nav=native_name_hash("ThrowdownPlacement");
        observe_flag_graph(0x9700be92);
        check(!native_setup_active() && nav==native_name_hash("ThrowdownPlacement"));
        const auto direct_count=navigations.size();nav=0;
        tick();
        check(placement().step==Step::placing && navigations.size()==direct_count);
        observe_flag_position(321,{1,2,3});observe_flag_exit(false);tick();
        check(handed_off.back().position==std::array<float,3>{1,2,3});
        current_view.state.match=88;current_view.state.phase=Phase::lobby;tick();
        const auto before_rejected=navigations.size();
        check(!arm_native_setup(30,1) && navigations.size()==before_rejected && nav==0);
        // A finished game may still own the native queue. A fresh click must
        // close it first, retain the request, then open exactly one 1-Up setup.
        current_view.state.phase=Phase::cancelled;
        check(arm_native_setup(20,1) && commands.back()=="leave");
        tick();check(destroyed.back()==321 && nav==0 && !native_setup_active());
        observe_flag_destroyed(321);test_now+=501;tick();
        check(native_setup_active() && nav==0 && placement().pending_setup==std::nullopt);
        tick();check(nav==native_name_hash("SpotBattle") && placement().seconds==20 && placement().players==1);
        nav=0;tick();check(navigations.size()==before_rejected+1); // No duplicate native navigation.
        abandon_flag_placement();
        // A native card can begin before the local capture is published. Flag
        // navigation must not cancel ownership simply because Steam is off.
        current_view.world=0;current_view.local=0;current_view.can_create=false;
        check(arm_native_setup(30,1));tick();nav=0;observe_flag_created();observe_flag_graph(0x0f131312);
        observe_flag_graph(0x9700be92);tick();check(flag_registration_owned() && placement().flag_seen);
        current_view.world=23;current_view.local=7;tick();
        check(placement().world==23 && placement().local==7 && placement().step==Step::placing);
        observe_flag_position(999,{2,3,4});observe_flag_exit(false);tick();
        check(handed_off.back().players==1 && handed_off.back().world==23 && handed_off.back().local==7);
        abandon_flag_placement();
        // Connecting while the native setup opens replaces offline IDs with
        // transport IDs. This is the same panel/level, not a stale flag.
        current_view.world=2;current_view.local=2;nav=0;
        check(arm_native_setup(30,1));tick();nav=0;
        const auto connecting_count=navigations.size();
        observe_flag_created();observe_flag_graph(0x0f131312);
        current_view.world=3715608209802941200ULL;current_view.local=76561198290301552ULL;
        tick();check(native_setup_active() && placement().created && placement().setup_seen);
        check(placement().world==current_view.world && placement().local==current_view.local && navigations.size()==connecting_count);
        observe_flag_graph(0x9700be92);tick();
        observe_flag_position(1000,{3,4,5});observe_flag_exit(false);tick();
        check(handed_off.back().world==current_view.world && handed_off.back().local==current_view.local);
        current_view.world=24;tick();check(!flag_registration_owned()); // Committed flags never rebind.
        check(arm_native_setup(20,1));tick();nav=0;
        abandon_flag_placement();check(!native_setup_active() && !flag_registration_owned());
        // Starting removes only the native lobby flag. The independently
        // coordinated 1-Up match and its saved facing remain intact.
        current_view.state={};nav=0;
        check(arm_native_setup(20,1));tick();nav=0;
        observe_flag_created();observe_flag_graph(0x0f131312);observe_flag_graph(0x9700be92);tick();
        observe_flag_position(1001,{7,8,9},&flag_transform);observe_flag_exit(false);tick();
        current_view.state.match=101;current_view.state.phase=Phase::countdown;tick();
        check(destroyed.back()==1001 && placement().destroy_queued);
        observe_flag_destroyed(1001);tick();
        check(current_view.state.match==101 && placement().mmid==0 && placement().facing==facing && !flag_registration_owned());
        const auto destroys=destroyed.size();current_view.state.phase=Phase::playing;tick();check(destroyed.size()==destroys);
        current_view.state.phase=Phase::cancelled;tick();tick();check(!flag_registration_owned());
        current_view.state={};nav=0;
        check(arm_native_setup(20,1));tick();nav=0;
        observe_flag_created();observe_flag_graph(0x0f131312);
        check(native_setup_active());
        observe_flag_graph(1867583346U);
        check(!native_setup_active()); // Back stops presentation adaptation immediately.
        const auto back_navigation_count=navigations.size();
        test_now+=1501;tick();
        check(!flag_registration_owned() && navigations.size()==back_navigation_count);
        observe_flag_created();observe_flag_graph(0x0f131312);
        check(!native_setup_active()); // A later stock Spot Battle cannot claim this cancelled entry.
        check(arm_native_setup(20,1));tick();nav=0;
        observe_flag_created();observe_flag_graph(0x0f131312);observe_flag_graph(1867583346U);
        observe_flag_graph(0x9700be92);test_now+=1501;tick();
        check(placement().flag_seen && flag_registration_owned()); // Confirm proceeds to flag placement.
        abandon_flag_placement();
        std::cout<<count<<" flag bridge checks passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
