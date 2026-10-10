#include "one_up_placement.h"
#include "one_up_runtime.h"
#include "native_throwdowns.h"
#include "throwdown_lab.h"
#include "Extension/UI/NativeMenu/native_menu_data.h"
#include "Extension/Modes/game_modes.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>

namespace dingosdk::multiplayer::one_up {
namespace {
enum class Step { idle, open, setup, flag, placing, closing, waiting, cleanup };
struct SetupRequest { unsigned seconds{}, players{}; std::uint64_t world{}, local{}; };
struct Placement {
    std::mutex mutex;
    Step step{};
    // Who the flag is for: 1-Up, or a game mode set up here (Extension/Modes), whose game key
    // `mode_game` is (the flag starts that game; when it is gone the flag goes too).
    bool for_mode{};
    std::uint64_t mode_game{};
    bool mode_start_requested{}, mode_stop_requested{};
    std::uint64_t menu_opened_at{}; // from the world: Throwdowns opened first, SpotBattle after it
    bool open_menu{};
    std::uint64_t world{}, local{}, until{}, changed{}, sent{}, reopen_until{}, reopen_at{};
    unsigned seconds{}, players{};
    std::uint32_t mmid{};
    bool created{}, setup_seen{}, flag_seen{}, exited{}, cancelling{}, destroy_queued{}, destroyed{};
    bool handed_off{}, match_seen{};
    bool show_setup{}, native_entry{};
    std::optional<std::array<float,3>> spawn;
    Facing facing=default_facing;
    std::optional<SetupRequest> pending_setup;
    std::string status;
};
Placement& placement() { static Placement p; return p; }
void reset(Placement& p) {
    p.step=Step::idle; p.spawn.reset(); p.facing=default_facing;p.mmid=0; p.sent=0;
    p.created=p.setup_seen=p.flag_seen=p.exited=p.cancelling=p.destroy_queued=p.destroyed=false;
    p.handed_off=p.match_seen=false;
    p.show_setup=p.native_entry=false;
    p.for_mode=false; p.mode_game=0; p.open_menu=false; p.menu_opened_at=0;
}
void open_native_setup(Placement& p,const SetupRequest& request,std::uint64_t now) {
    reset(p); p.pending_setup.reset();
    p.step=Step::open; p.show_setup=p.native_entry=true;
    p.world=request.world; p.local=request.local;
    p.seconds=request.seconds; p.players=request.players;
    p.changed=now; p.until=now+10000;
    p.reopen_until=p.reopen_at=0; p.status.clear();
}
// QueueStateNavigation's live graph publishes these two fields, with metadata
// [-1] for a command without arguments. Do not change the Back keyword stack.
bool navigate(const menu_data::Context& c, std::string_view name, std::uint32_t consumed=0) {
    using namespace menu_data;
    const auto roots=c.roots({0xcc4776b5});
    require(roots.size()==1,"Native navigation model is unavailable.");
    const auto current=c.field(roots.front().model,0xf3ba9cf8);
    const auto metadata=c.field(roots.front().model,0xf437b255);
    const auto pending=read<std::uint32_t>(c.address(current));
    if(pending && pending!=consumed) return false;
    c.set(current,game::native_name_hash(name));
    c.array(metadata,std::vector<std::int32_t>{-1});
    logging::log(logging::Level::info,logging::Channel::ui,"1-Up flag: navigation {}.",name);
    return true;
}
}
bool begin_flag_placement(unsigned seconds,unsigned players,bool show_native_setup) {
    const auto v=view();
    if(!v.can_create || v.state.match || seconds<10 || seconds>120 || players<1 || players>6) return false;
    auto& p=placement(); std::lock_guard lock(p.mutex);
    if(p.step!=Step::idle) return false;
    reset(p); p.step=Step::open; p.world=v.world; p.local=v.local;
    p.show_setup=show_native_setup;
    p.reopen_until=p.reopen_at=0;
    p.seconds=seconds; p.players=players; p.changed=GetTickCount64(); p.until=p.changed+15000;
    p.status="Opening flag placement...";
    prepare_throwdown_injection(); // Prewarm cleanup before the player places the flag.
    return true;
}
bool begin_mode_flag_placement(bool open_menu) {
    const auto v=view();
    const auto game=modes::flag_game();
    if(!game || !v.world || v.state.match) return false;
    auto& p=placement(); std::lock_guard lock(p.mutex);
    if(p.step!=Step::idle) return false;
    reset(p); p.step=Step::open; p.world=v.world; p.local=v.local;
    p.for_mode=true; p.mode_game=game; p.open_menu=open_menu;
    // The stock two steps, as 1-Up's card: the setup (dressed as the mode) and its Confirm, then the flag.
    p.show_setup=true;
    p.reopen_until=p.reopen_at=0;
    p.seconds=20; p.players=6; p.changed=GetTickCount64(); p.until=p.changed+15000;
    p.status="Opening flag placement...";
    prepare_throwdown_injection();
    return true;
}
bool flag_for_mode() noexcept { auto& p=placement(); std::lock_guard lock(p.mutex); return p.step!=Step::idle && p.for_mode; }
void request_mode_start() noexcept { auto& p=placement(); std::lock_guard lock(p.mutex); if(p.for_mode) p.mode_start_requested=true; }
void request_mode_stop() noexcept { auto& p=placement(); std::lock_guard lock(p.mutex); if(p.for_mode) p.mode_stop_requested=true; }
ModeFlagRequests take_mode_flag_requests() noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    ModeFlagRequests r{p.mode_start_requested,p.mode_stop_requested};
    p.mode_start_requested=p.mode_stop_requested=false;
    return r;
}
bool arm_native_setup(unsigned seconds,unsigned players) {
    const auto v=view();
    const bool ended=v.state.phase==Phase::finished || v.state.phase==Phase::cancelled;
    if(seconds<10 || seconds>120 || players<1 || players>6 || (v.state.match && !ended))return false;
    auto& p=placement();
    {
        std::lock_guard lock(p.mutex);
        const SetupRequest request{seconds,players,v.world,v.local};
        if(p.step==Step::cleanup || p.step==Step::waiting) {
            // The old native queue still owns a flag. Finish its teardown
            // before opening the next setup, without losing this card click.
            if(p.step==Step::waiting && p.handed_off && !p.match_seen && !ended)return false;
            p.pending_setup=request;
            if(p.step!=Step::cleanup) {
                p.step=Step::cleanup;p.cancelling=true;p.until=GetTickCount64()+15000;
            }
        } else {
            if(p.step!=Step::idle && p.step!=Step::open &&
               !(p.step==Step::setup && !p.flag_seen && !p.mmid))return false;
            open_native_setup(p,request,GetTickCount64());
        }
    }
    if(v.state.match)queue("leave");
    prepare_throwdown_injection();
    return true;
}
bool native_setup_active() noexcept { auto& p=placement(); std::lock_guard lock(p.mutex); return !p.for_mode && p.show_setup && (p.step==Step::open || p.step==Step::setup); }
void set_native_setup_options(unsigned seconds,unsigned players) noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    if(p.step==Step::setup && seconds>=10 && seconds<=120 && players>=1 && players<=6) { p.seconds=seconds; p.players=players; }
}
bool flag_placement_active() noexcept { auto& p=placement(); std::lock_guard lock(p.mutex); return p.step!=Step::idle && p.step!=Step::waiting; }
bool flag_registration_owned() noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    // The running match outlives its temporary native flag registration.
    // Once that registration is destroyed, stock challenges own their events.
    return p.step!=Step::idle && (p.step!=Step::waiting || p.mmid!=0);
}
bool owns_flag_registration(std::uint32_t mmid) noexcept { auto& p=placement(); std::lock_guard lock(p.mutex); return mmid && p.step!=Step::idle && p.mmid==mmid; }
void flag_registration_left() noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    if(p.step==Step::idle)return;
    p.cancelling=true; p.step=Step::cleanup; p.until=GetTickCount64()+15000;
}
std::string flag_placement_status() { auto& p=placement(); std::lock_guard lock(p.mutex); return p.status; }
bool flag_lobby_requested() noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    const auto now=GetTickCount64(); return p.reopen_at && now>=p.reopen_at && now<p.reopen_until;
}
void flag_lobby_shown() noexcept { auto& p=placement(); std::lock_guard lock(p.mutex); p.reopen_until=p.reopen_at=0; }
void observe_flag_graph(std::uint32_t graph) noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    if(p.step==Step::idle) return;
    if(graph==0x0f131312 && p.step==Step::setup) { // Spot Battle setup OnBegin
        p.setup_seen=true; p.until=GetTickCount64()+300000;
    }
    if(graph==1867583346U && p.step==Step::setup && p.setup_seen) { // Same setup OnEnd
        // Back and Confirm both leave this native state. Stop adapting its UI
        // immediately; a successful Confirm enters the flag update below.
        p.show_setup=false; p.changed=GetTickCount64(); p.until=p.changed+1500;
    }
    // Spot Battle's own placement update runs after its validator/spawn
    // initialization. Its Confirm goes directly here; no S.K.A.T.E. area
    // state, skip predicate, or synthetic navigation is involved.
    if(graph==0x9700be92 && p.created && p.setup_seen &&
       (p.step==Step::setup || (p.step==Step::flag && !p.flag_seen))) {
        p.step=Step::flag; p.flag_seen=true; p.until=GetTickCount64()+300000;
        logging::write(logging::Level::info,logging::Channel::ui,"1-Up flag: entered native Spot Battle flag placement.");
    }
}
void observe_flag_created() noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    if(p.step==Step::setup) p.created=true;
}
void observe_flag_position(std::uint32_t mmid,const std::array<float,3>& spawn,const std::array<float,16>* transform) noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    if((p.step!=Step::placing && p.step!=Step::flag) || !p.created || !p.flag_seen || !mmid ||
       !std::all_of(spawn.begin(),spawn.end(),[](float n){return std::isfinite(n) && std::abs(n)<100000;})) return;
    if(transform) {
        const auto facing=transform_facing(*transform);
        if(!valid_facing(facing))return;
        p.facing=facing;
    }
    p.spawn=spawn; p.mmid=mmid; p.step=Step::closing; p.until=GetTickCount64()+15000;
    p.status="Saving the starting flag...";
    logging::log(logging::Level::info,logging::Channel::ui,"1-Up flag: accepted spawn ({:.2f}, {:.2f}, {:.2f}), native queue {:#x}.",spawn[0],spawn[1],spawn[2],mmid);
}
void observe_flag_exit(bool cancelling) noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    if(p.step==Step::idle) return;
    p.exited=true; p.cancelling=cancelling || !p.spawn;
    p.step=p.cancelling?Step::cleanup:Step::waiting; p.changed=GetTickCount64(); p.until=p.changed+15000;
    if(p.cancelling) p.status="Flag placement cancelled.";
}
void observe_flag_destroyed(std::uint32_t mmid) noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    if(p.step==Step::waiting && p.handed_off && p.destroy_queued && p.mmid==mmid && p.for_mode) {
        p.mmid=0;p.destroyed=true;p.changed=GetTickCount64();return;
    }
    if(p.step==Step::waiting && p.handed_off && p.destroy_queued && p.mmid==mmid) {
        // The registration is a lobby flag, not the running 1-Up match. Once
        // its transform is saved, use the native teardown to remove the pole,
        // ground decal and waiting notification without ending 1-Up.
        p.mmid=0;p.destroyed=true;p.changed=GetTickCount64();return;
    }
    if(p.step==Step::cleanup && p.mmid==mmid) { p.destroyed=true; p.changed=GetTickCount64(); }
}
void abandon_flag_placement() noexcept {
    auto& p=placement(); std::lock_guard lock(p.mutex);
    // The level's own teardown removes its queues. Never destroy an old MMID
    // in a new world, where the native coordinator can reuse it.
    if(p.step!=Step::idle) {
        if(p.created) release_one_up_native_placeholder();
        reset(p); p.status="Flag placement ended because the map changed.";
    }
    p.pending_setup.reset();p.reopen_until=p.reopen_at=0;
}
void tick_flag_placement(const menu_data::Context& c) {
    const auto v=view(); const auto now=GetTickCount64();
    auto& p=placement();
    bool reopen{};
    {
        std::lock_guard lock(p.mutex);
        if(p.step==Step::idle) {
            reopen=p.reopen_until>now && !p.reopen_at && v.state.match && v.world==p.world && v.local==p.local;
            if(!reopen) return;
        }
    }
    if(reopen) {
        // Toolbox_RadialListItem_Throwdowns uses this same navigation id.
        if(navigate(c,"Throwdowner")) { std::lock_guard lock(p.mutex); p.reopen_at=now+500; }
        return;
    }
    std::string command; Step next{}; std::uint32_t destroy{};
    std::optional<SpawnRequest> handoff;
    std::optional<std::pair<std::array<float,3>,float>> mode_flag;
    bool release{};
    {
        std::lock_guard lock(p.mutex);
        if(p.step==Step::idle) return;
        // The same native setup survives connecting to a session. Offline
        // player/level IDs can change to transport IDs before the flag is
        // placed; keep ownership and bind the eventual spawn to that session.
        // Real level teardown calls abandon_flag_placement before unloading.
        if(p.native_entry && !p.handed_off && !p.spawn && p.step!=Step::cleanup) {
            if(v.world && v.local) { p.world=v.world; p.local=v.local; }
            else if(now<p.until && p.step!=Step::open)return; // Setup can open before capture initializes.
        } else if(v.world!=p.world || v.local!=p.local || !v.world) {
            if(p.created) release_one_up_native_placeholder();
            logging::log(logging::Level::warning,logging::Channel::ui,
                "1-Up flag: session changed during step {}, world {} -> {}, player {} -> {}.",
                static_cast<unsigned>(p.step),p.world,v.world,p.local,v.local);
            reset(p);p.pending_setup.reset(); p.reopen_until=p.reopen_at=0; p.status="Flag placement ended because the session changed."; return;
        }
        if(p.step==Step::flag && p.flag_seen) { p.step=Step::placing; p.until=now+300000; p.status="Place your starting flag. Score anywhere."; }
        if(p.step==Step::waiting && p.for_mode) {
            // A game mode's flag: its spot and facing go to the game; the registration stays as
            // the world flag with its waiting card until the game counts down, then goes.
            if(!p.handed_off && p.spawn) {
                mode_flag={*p.spawn,std::atan2(p.facing[6],p.facing[8])*180.f/3.14159265f};
                p.handed_off=true; p.status.clear(); release=true;
            }
            const auto m=modes::native_match();
            const bool same=modes::flag_game()==p.mode_game || (m.active && m.key==p.mode_game);
            if(p.handed_off && p.mmid && !p.destroy_queued && same && (m.phase==2 || m.phase==3)) destroy=p.mmid;
            // The native waiting card can outlive its destroyed queue: it stays this game's (its
            // name, LEAVE / END GAME) until the game is over, never Spot Battle's again.
            if(!same) { p.step=Step::cleanup; p.cancelling=true; p.until=now+15000; }
        }
        else if(p.step==Step::waiting) {
            // Keep the native registration alive: it owns the world flag,
            // waiting notification, and Start/Leave input prompts.
            if(!p.handed_off && p.spawn) {
                handoff=SpawnRequest{*p.spawn,p.world,p.local,p.seconds,p.players,p.facing};
                p.handed_off=true; p.status.clear(); release=true;
            }
            if(v.state.match) p.match_seen=true;
            if(p.handed_off && p.match_seen && p.mmid && !p.destroy_queued &&
               (v.state.phase==Phase::countdown || v.state.phase==Phase::playing))destroy=p.mmid;
            if((p.match_seen && (!v.state.match || v.state.phase==Phase::cancelled)) ||
               (p.handed_off && !p.match_seen && now>=p.until)) {
                p.step=Step::cleanup; p.cancelling=true; p.until=now+15000;
            }
        }
        if(p.step==Step::setup && p.setup_seen && !p.show_setup && p.native_entry && now>=p.until) {
            release=p.created; reset(p); p.status.clear();
            // Back already performed the native navigation. Do not send an
            // exit that could close the next stock challenge's setup.
            if(release)release_one_up_native_placeholder();
            return;
        }
        if((p.step==Step::open || p.step==Step::setup) && !p.created && !p.setup_seen && now>=p.until) {
            reset(p); p.status="The setup did not open. Select 1-Up to try again.";
            return; // No native queue was created; do not close another menu.
        }
        if(now>=p.until && p.step!=Step::cleanup && p.step!=Step::waiting) {
            p.cancelling=true; p.spawn.reset(); p.status="Flag placement timed out. Open 1-Up to try again.";
            p.step=Step::cleanup; p.until=now+15000; p.sent=0;
        }
        if(p.step==Step::open && p.open_menu && !p.menu_opened_at) { command="Throwdowner"; next=Step::open; p.menu_opened_at=now; p.until=now+15000; }
        else if(p.step==Step::open && p.open_menu && now<p.menu_opened_at+900) {} // Throwdowns still opening
        else if(p.step==Step::open) { command="SpotBattle"; next=Step::setup; }
        else if(p.step==Step::setup && p.created && p.setup_seen && !p.show_setup) { command="ThrowdownPlacement"; next=Step::flag; }
        else if(p.step==Step::cleanup) {
            if(!p.exited && !p.sent) { command="ThrowdownerExit"; next=Step::cleanup; }
            if(p.mmid && !p.destroy_queued) destroy=p.mmid;
            if((p.exited && !p.mmid) || (p.destroyed && now>=p.changed+500)) {
                release=p.created;
                const auto requested=p.pending_setup;
                reset(p);p.pending_setup.reset();p.status.clear();
                if(requested && requested->world==v.world && requested->local==v.local)
                    open_native_setup(p,*requested,now);
            } else if(now>=p.until) {
                // Do not create 1-Up if the native queue could not be removed.
                p.status="Could not close flag placement. Back out of the native Throwdown before retrying.";
            }
        }
    }
    if(release) release_one_up_native_placeholder();
    if(handoff) create_at_spawn(*handoff);
    if(mode_flag) {
        const auto said=modes::flag_placed(mode_flag->first,mode_flag->second);
        logging::log(logging::Level::info,logging::Channel::ui,"Game modes flag: {}",said);
    }
    if(destroy && prepare_throwdown_injection() && queue_throwdown_destroy(destroy)) {
        std::lock_guard lock(p.mutex); if(p.mmid==destroy) p.destroy_queued=true;
    }
    if(!command.empty()) {
        // Publication can run expression callbacks. Arm the expected step
        // before publishing, without holding this mutex across native code.
        Step previous;
        { std::lock_guard lock(p.mutex); previous=p.step; p.step=next; }
        bool sent{};
        try { sent=navigate(c,command); }
        catch(...) { std::lock_guard lock(p.mutex); p.step=previous; throw; }
        std::lock_guard lock(p.mutex);
        if(!sent) p.step=previous;
        else { p.sent=now; if(next!=Step::cleanup) p.until=now+15000; }
    }
}
}
