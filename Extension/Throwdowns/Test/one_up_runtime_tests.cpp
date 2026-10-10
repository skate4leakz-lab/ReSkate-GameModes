#include <Windows.h>
static ULONGLONG test_now{};
static ULONGLONG one_up_test_clock() { return test_now; }
#define GetTickCount64 one_up_test_clock
#include "Extension/Throwdowns/one_up_runtime.cpp"
#include "Extension/Throwdowns/one_up_local_input.h"
#undef GetTickCount64
#include <iostream>
#include <stdexcept>
#include <source_location>
static bool test_teleport_pending{};
static std::vector<std::array<float,3>> test_teleports;
static std::vector<std::array<float,16>> test_spawn_transforms;
namespace dingosdk {
bool teleport_local_skater(const std::array<float,3>& position) { test_teleports.push_back(position);return true; }
bool teleport_local_skater_transform(const std::array<float,16>& transform) { test_teleports.push_back({transform[12],transform[13],transform[14]});test_spawn_transforms.push_back(transform);return true; }
bool local_skater_teleport_pending() { return test_teleport_pending; }
}
namespace dingosdk::multiplayer {
bool local_throwdown_active() noexcept { return false; }
void spectate_party_member(std::uint64_t) noexcept {}
namespace one_up {
static std::vector<NativeLine> test_lines;
bool native_lines_available() noexcept { return true; }
void prepare_native_lines(std::uintptr_t, std::uint64_t, bool) noexcept {}
void update_native_scoring_rule(std::uintptr_t, std::uint64_t, bool) noexcept {}
void arm_native_lines(Token, bool, Time) noexcept {}
std::vector<NativeLine> take_native_lines() { return std::exchange(test_lines, {}); }
}
}
using namespace dingosdk::multiplayer;
using namespace dingosdk::multiplayer::one_up;
int main() {
    try {
        const PlayerId a = 76561198000000001, b = a + 1;
        Runtime r; r.input.local = a; r.input.world = 10; r.input.in_world = true; r.input.position = std::array<float,3>{1,2,3};
        ThrowdownPeer peer; peer.id = b; peer.position = r.input.position; r.input.peers.push_back(peer);
        unsigned count{};
        const auto check = [&](bool ok,const std::source_location where=std::source_location::current()) { ++count; if (!ok) throw std::runtime_error("Coordinator check " + std::to_string(count)+" at line "+std::to_string(where.line())); };
        ThrowdownRelayInput disconnected;disconnected.in_world=true;disconnected.local_name="Local skater";
        const auto practice=local_input(disconnected,7,12,std::array<float,3>{1,2,3});
        check(practice.local==7 && practice.world==12 && practice.in_world && practice.position && practice.peers.empty() && practice.local_only);
        check(disconnected.local==0 && disconnected.world==0 && !disconnected.position); // Stock relay is unchanged.
        check(!local_input(disconnected,0,12,std::array<float,3>{1,2,3}).in_world);
        check(!local_input(disconnected,7,12,std::nullopt).in_world);
        check(local_input(disconnected,7,13,std::array<float,3>{1,2,3}).world!=practice.world); // Same-map reload expires the match.
        command(r, "create 30", 100); check(r.hosted.has_value() && r.spot == *r.input.position);
        Message m; m.match = r.selected.match; m.world = 10; m.leader = a; m.action = Action::join; m.sequence = 1;
        apply_input(r, b + 1, m, 101); check(r.hosted->state().players.size() == 1);
        m.world = 11; apply_input(r, b, m, 101); check(r.hosted->state().players.size() == 1);
        m.world = 10; apply_input(r, b, m, 101); apply_input(r, b, m, 102); check(r.hosted->state().players.size() == 2);
        m.action = Action::ready; m.sequence = 2; m.flag = true;
        r.input.peers[0].position = std::array<float,3>{50,2,3}; apply_input(r, b, m, 103); check(!r.hosted->state().players[1].ready);
        m.sequence = 3; r.input.peers[0].position = r.input.position; apply_input(r, b, m, 104); check(r.hosted->state().players[1].ready);
        m.sequence = 1; apply_input(r, a, m, 105); command(r, "start", 200); check(r.hosted->state().phase == Phase::countdown);
        r.hosted->position_confirmed(r.hosted->token(), 300); r.hosted->tick(300+r.hosted->state().config.countdown_ms); const auto old = r.hosted->token();
        m.action = Action::trick_score; m.sequence = 2; m.turn = old.turn; m.line = 1; m.elapsed = 100;
        auto end = m; end.action = Action::trick_score; end.sequence = 3; end.score = 321.25; end.elapsed = 200;
        r.input.position = std::array<float,3>{1001,2,3003};
        apply_input(r, a, end, 6500); check(r.hosted->state().best == 0);
        apply_input(r, a, m, 6501); check(r.hosted->state().best == 321.25); // Scoring anywhere, far outside the former circle.
        apply_input(r, a, end, 6502); check(r.hosted->state().best == 321.25);
        Message snapshot; snapshot.match = r.selected.match; snapshot.world = 10; snapshot.leader = a; snapshot.state = r.hosted->state(); snapshot.spot = r.spot;
        emit(r, snapshot); const auto envelope = decode_throwdown(r.outgoing.front()); check(envelope && one_up::decode(envelope->one_up) == snapshot);
        r.hosted->leave(b, 3800); r.selected = r.hosted->state(); check(r.selected.winner == a);
        command(r, "rematch", 3900); check(r.hosted->state().phase == Phase::lobby);
        r.hosted->join(b); r.hosted->ready(a, true); r.hosted->ready(b, true); r.hosted->start(4000); r.hosted->tick(4000+r.hosted->state().config.countdown_ms);
        check(r.hosted->token().turn > old.turn && !r.hosted->trick_scored(old, 2, 100, 100, 7100));
        r.sequence = 3; command(r, "leave", 7200); check(!r.selected.match && !r.hosted && r.outgoing.size() > 1);
        command(r, "create 40 2", 7201); check(r.hosted && r.hosted->state().config.max_players == 2 && r.hosted->state().config.turn_ms == 40000);
        clear(r);
        const SpawnRequest picked{{51,203,-709},10,a,50,6,{0,0,-1,0,1,0,1,0,0}};
        create_selected_spawn(r,picked,7202);
        check(r.hosted && r.spot==picked.position && r.spot!=*r.input.position);
        check(r.facing==picked.facing);
        check(r.selected.config.max_players==6 && r.selected.config.turn_ms==50000 && r.ready_pending);
        const auto placed_match=r.selected.match;
        create_selected_spawn(r,SpawnRequest{{0,0,0},10,a,30,4},7203);
        check(r.selected.match==placed_match && r.spot==picked.position);
        clear(r);
        auto stale=picked; stale.world=11; create_selected_spawn(r,stale,7204); check(!r.hosted);
        stale=picked; stale.local=b; create_selected_spawn(r,stale,7205); check(!r.hosted);
        stale=picked; stale.position[0]=std::numeric_limits<float>::infinity(); create_selected_spawn(r,stale,7206); check(!r.hosted);
        stale=picked; stale.players=0; create_selected_spawn(r,stale,7207); check(!r.hosted);
        stale=picked; stale.seconds=0; create_selected_spawn(r,stale,7208); check(!r.hosted);
        command(r,"create 30",7210); r.hosted->ready(a,true);
        command(r,"solo",7211); check(!r.hosted->solo_test()); // A connected peer blocks isolated practice.
        r.input.peers.clear(); command(r,"solo",7212);
        check(r.hosted->solo_test() && r.hosted->state().phase==Phase::countdown);
        r.outgoing.clear(); emit(r,snapshot); check(r.outgoing.empty()); // Never advertise solo practice as a normal match.
        clear(r); command(r,"create 30",7213); r.hosted->ready(a,true);
        command(r,"start",7214);
        check(r.hosted->solo_test() && r.hosted->state().phase==Phase::countdown);
        clear(r); r.input.peers.push_back(peer);
        auto solo_spawn=picked;solo_spawn.players=1;
        create_selected_spawn(r,solo_spawn,7215);check(r.hosted && r.hosted->state().config.max_players==1);
        r.hosted->ready(a,true);command(r,"start",7216);
        check(r.hosted->solo_test() && r.hosted->state().phase==Phase::countdown);
        clear(r);
        r.input.position = std::array<float,3>{1,2,3};
        // Exercise the actual per-frame coordinator, including native-event delivery,
        // clock transitions, position confirmation and authenticated remote input.
        auto input = r.input;
        input.local_name="Test skater";
        test_now = 1000; queue("create 30"); tick(0, input);
        auto v = view(); check(v.state.match && v.state.phase == Phase::lobby);
        check(v.name(a)=="Test skater");
        input.local_name="Unknown Player";tick(0,input);
        check(view().name(a)=="Test skater"); // Transient identity refresh cannot erase a known name.
        input.local_name="Renamed skater";tick(0,input);
        check(view().name(a)=="Renamed skater"); // Real persona changes still propagate.
        Message guest; guest.match = v.state.match; guest.world = input.world; guest.leader = a;
        guest.action = Action::join; guest.sequence = 1;
        const auto deliver = [&](const Message& message) {
            ThrowdownMessage packet; packet.kind = ThrowdownMessage::Kind::one_up;
            packet.leader = a; packet.id = static_cast<std::uint32_t>(message.match) | 1U;
            packet.one_up = one_up::encode(message); receive(b, encode_throwdown(packet));
        };
        deliver(guest); guest.action = Action::ready; guest.sequence = 2; guest.flag = true; deliver(guest);
        queue("ready"); test_now = 1001; tick(0, input);
        check(view().state.players.size() == 2 && view().state.players[0].ready && view().state.players[1].ready);
        queue("start"); test_now = 1002; tick(0, input); check(view().state.phase == Phase::countdown && !waits_offboard());
        check(countdown_locks_input());
        check(view().state.config.countdown_ms==6000);
        test_now=4002; tick(0,input);
        check(view().state.phase==Phase::countdown && countdown_locks_input()); // Native mode intro finishes; 3-2-1 still locks movement.
        test_now = 7002; tick(0, input); v = view();
        check(v.state.phase == Phase::playing && v.state.active == a && !waits_offboard() && hides(b));
        check(!countdown_locks_input());
        const Token frame_token{v.state.match, v.state.turn, a};
        input.position = std::array<float,3>{1001,2,3003}; // Flag is a spawn, never a scoring boundary.
        test_lines.push_back({frame_token, 1, 200, 8, true, true, true}); test_now = 7010; tick(0, input);
        check(view().state.best==200); // No line-end event is required.
        test_lines.push_back({frame_token,2,250,100,true,true,true});test_now=7102;tick(0,input);
        check(view().state.best==450);
        test_now=37002;tick(0,input);check(view().state.phase==Phase::settling);
        test_lines.push_back({frame_token,3,99999,30000,true,true,true});test_now=37003;tick(0,input);
        check(view().state.best==450);
        test_now=38502;tick(0,input);check(view().state.phase==Phase::feedback && view().state.target==450);
        test_now = 46002; tick(0, input); test_now = 46003; tick(0, input);
        test_now = 52003; tick(0, input); v = view();
        check(v.state.phase == Phase::playing && v.state.active == b && waits_offboard() && !hides(b));
        input.peers[0].position = std::array<float,3>{-1001,2,-3003};
        guest.action = Action::trick_score; guest.sequence = 3; guest.turn = v.state.turn; guest.line = 1; guest.elapsed = 10;
        auto remote_end = guest; remote_end.action = Action::trick_score; remote_end.sequence = 4; remote_end.elapsed = 100; remote_end.score = 451; remote_end.flag = true;
        deliver(remote_end); test_now = 52103; tick(0, input); check(view().state.best == 0);
        deliver(guest); test_now = 52104; tick(0, input); check(view().state.best == 451);
        test_lines.push_back({frame_token, 2, 99999, 100, true, true, true}); test_now = 52105; tick(0, input);
        check(view().state.best == 451);
        input.world = 11; test_now = 52106; tick(0, input); check(!view().state.match && !waits_offboard() && !hides(a));
        create_at_spawn(picked); test_now=52107; tick(0,input); check(!view().state.match); // Delayed flag from a departed map.
        auto current=picked; current.world=11;
        create_at_spawn(current); test_now=52108; tick(0,input);
        check(view().state.match && view().world==11 && runtime().spot==picked.position && view().state.config.max_players==6);
        input.peers.clear(); input.position=picked.position;
        test_now=52608; tick(0,input); test_now=52609; tick(0,input);
        queue("solo"); test_now=52610; check(tick(0,input).empty() && view().solo_test && view().state.phase==Phase::countdown);
        test_now=58610; tick(0,input); check(view().state.phase==Phase::playing && !waits_offboard());
        input.peers.push_back(peer); test_now=58611; tick(0,input);
        check(!view().state.match && !view().solo_test && !waits_offboard());
        current.players=1;create_at_spawn(current);test_now=58612;tick(0,input);
        check(view().state.config.max_players==1 && view().state.players[0].ready);
        queue("start");test_now=58613;tick(0,input);
        check(view().solo_test && view().state.phase==Phase::countdown && countdown_locks_input());
        test_now=64613;tick(0,input);
        check(view().state.phase==Phase::playing && !countdown_locks_input());
        input.peers.push_back({b+1});test_now=64614;tick(0,input);
        check(view().solo_test && view().state.players.size()==1); // Other session players cannot interrupt selected solo play.
        clear(runtime());
        auto offline=practice;offline.position=std::array<float,3>{20,2,3};
        tick(0,offline);check(view().can_create && view().local==7 && view().world==12);
        create_at_spawn(SpawnRequest{{1,2,3},12,7,30,1});test_now=65000;check(tick(0,offline).empty());
        check(view().state.match && !view().state.players.front().ready);
        queue("start");test_now=65001;tick(0,offline);
        check(view().state.phase==Phase::lobby && runtime().start_pending); // Fast Start waits for flag arrival.
        offline.position=std::array<float,3>{1,2,3};test_now=65501;tick(0,offline);
        test_now=65502;tick(0,offline);
        check(view().solo_test && view().state.phase==Phase::countdown && countdown_locks_input());
        test_now=71502;tick(0,offline);check(view().state.phase==Phase::playing);
        const Token offline_turn{view().state.match,view().state.turn,offline.local};
        offline.position=std::array<float,3>{101,2,303};
        test_lines.push_back({offline_turn,1,275,100,true,true,true});test_now=71602;
        check(tick(0,offline).empty() && view().state.best==275); // Native IDs score locally, never enter the Steam wire.
        test_lines.push_back({offline_turn,1,275,100,true,true,true});test_now=71603;tick(0,offline);
        check(view().state.best==275); // A repeated landing cannot award points twice.
        offline.world=13;tick(0,offline);check(!view().state.match && !countdown_locks_input());
        offline.position=std::array<float,3>{1,2,3};
        create_at_spawn(SpawnRequest{{1,2,3},13,7,30,1});test_now=72000;tick(0,offline);
        test_now=72001;tick(0,offline);check(view().state.players.front().ready);
        test_teleport_pending=true;
        queue("start");test_now=72002;tick(0,offline);
        check(view().positioning && countdown_locks_input());
        // The coordinates can already match while the native ground check is
        // still running. No intro or 3-2-1 may consume time during that work.
        test_now=76002;tick(0,offline);
        check(view().positioning && view().remaining==6000 && view().state.phase==Phase::countdown);
        test_teleport_pending=false;test_now=76003;tick(0,offline);
        check(!view().positioning && view().remaining==6000);
        test_now=79003;tick(0,offline);check(view().remaining==3000 && countdown_locks_input());
        test_now=82003;tick(0,offline);check(view().state.phase==Phase::playing && !countdown_locks_input());
        // Follow a scored solo attempt through every subsequent turn and
        // elimination. The skater moves away from the flag each turn; native
        // arrival must hold the next countdown instead of ending the game.
        clear(runtime());test_teleports.clear();test_now=90000;
        const auto flag=std::array<float,3>{1,2,3};offline.position=flag;
        const Facing turn_facing{0,0,-1,0,1,0,1,0,0};
        create_at_spawn(SpawnRequest{flag,13,7,10,1,turn_facing});tick(0,offline);
        ++test_now;tick(0,offline);check(view().state.players.front().ready);
        queue("start");++test_now;tick(0,offline);
        test_now+=6000;tick(0,offline);
        for(unsigned turn=1;turn<=4;++turn) {
            const auto current_view=view();
            check(current_view.solo_test && current_view.state.phase==Phase::playing && current_view.state.turn==turn);
            check(!test_spawn_transforms.empty() && transform_facing(test_spawn_transforms.back())==turn_facing);
            const Token current_token{current_view.state.match,current_view.state.turn,offline.local};
            const auto started=test_now;
            if(turn<=2) {
                test_lines.push_back({current_token,1,100,1,true,true,true});++test_now;tick(0,offline);
                check(view().state.best==100); // First target, then a tie earning 1.
            }
            offline.position=std::array<float,3>{101,2,303};
            test_now=started+10000;tick(0,offline);check(view().state.phase==Phase::settling);
            test_now+=1500;tick(0,offline);
            check(view().state.players.front().penalties==turn-1);
            if(turn==4) {
                check(view().state.phase==Phase::cancelled && view().state.players.front().penalties==3 && !countdown_locks_input());
                break;
            }
            check(view().state.phase==Phase::feedback);
            test_now+=2500;tick(0,offline);check(view().state.phase==Phase::countdown && countdown_locks_input() && view().positioning);
            const auto requests=test_teleports.size();test_teleport_pending=true;
            ++test_now;tick(0,offline);
            check(test_teleports.size()==requests+1 && test_teleports.back()==flag && view().positioning);
            test_now+=1000;tick(0,offline);
            check(view().state.phase==Phase::countdown && view().remaining==6000 && test_teleports.size()==requests+1);
            offline.position=flag;test_teleport_pending=false;++test_now;tick(0,offline);
            check(!view().positioning && view().remaining==6000);
            test_now+=6000;tick(0,offline);
        }
        std::cout << count << " 1-Up coordinator checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
