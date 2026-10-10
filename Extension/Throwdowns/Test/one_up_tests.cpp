#include "Extension/Throwdowns/one_up_match.h"
#include "Extension/Throwdowns/one_up_wire.h"
#include "Extension/Throwdowns/one_up_skating_score.h"
#include "Engine/Game/Build/20260929/one_up_scoring.h"
#include <iostream>
#include <stdexcept>
#include <limits>
using namespace dingosdk::multiplayer::one_up;
namespace {
constexpr PlayerId a = 76561198000000001, b = a + 1, c = a + 2;
unsigned checks{};
void check(bool ok, const char* text) { ++checks; if (!ok) throw std::runtime_error(text); }
Match match(bool third = false) {
    Config config;config.turn_ms=30000;
    Match m(100, 200, a,config); m.join(b); if (third) m.join(c); m.ready(a, true); m.ready(b, true); if (third) m.ready(c, true); m.start(0); m.tick(3000); return m;
}
void finish_turn(Match& m, double score, bool landed = true) {
    const auto at = m.turn_at(); const auto t = m.token();
    if (score > 0 && landed) check(m.trick_scored(t, 1, score, 1000, at + 1000), "landed trick banks immediately");
    m.tick(at + 36500);
}
void next(Match& m) { m.tick(m.phase_at() + 2500); m.tick(m.phase_at() + 3000); }
}
int main() {
    try {
        check(Config{}.turn_ms==20000,"new matches default to twenty-second turns");
        check(skating_sequence(1,skating_trick_category(1)) && skating_sequence(1,skating_trick_category(5)),"flips and manuals are eligible skating");
        check(!skating_sequence(1,skating_trick_category(6)),"riding, rolls and trickless air do not score");
        check(!skating_sequence(2,true) && !skating_sequence(3,true),"on-foot and wipeout sequences do not score");
        check(!skating_trick_category(11) && !skating_trick_category(12),"bonus and challenge actions do not score");
        Match lobby(1, 2, a); check(!lobby.start(0), "two players required"); lobby.join(b);
        check(!lobby.start(0), "all ready required"); check(!lobby.join(a), "no duplicate roster");
        Config limit; limit.max_players = 2;
        Match pair(2, 3, a, limit);
        check(pair.join(b) && !pair.join(c), "selected player limit is enforced by the host");
        limit.max_players = 1; check(limit.valid(), "one-player selection accepted");
        Match selected_solo(5,6,a,limit);
        check(!selected_solo.join(b) && !selected_solo.start(0), "one-player limit excludes guests and still requires readiness");
        selected_solo.ready(a,true);
        check(selected_solo.start(0) && selected_solo.solo_test(), "Players 1 uses normal Start for solo play");
        Message solo_snapshot; solo_snapshot.match=selected_solo.state().match;solo_snapshot.world=selected_solo.state().world;
        solo_snapshot.leader=a;solo_snapshot.state=selected_solo.state();
        check(decode(encode(solo_snapshot))==solo_snapshot, "one-player configuration survives the wire format");
        limit.max_players = 0; check(!limit.valid(), "zero-player limit rejected");
        limit.max_players = 7; check(!limit.valid(), "more than six players rejected");
        Match partial(3,4,a); partial.join(b); partial.ready(a,true); partial.ready(b,true);
        check(partial.start(0), "host can start with two ready skaters and four open slots");
        Match solo(4,5,a);
        check(!solo.start_solo_test(0),"solo test requires ready confirmation");
        solo.ready(a,true);
        check(!solo.start(0) && solo.start_solo_test(0),"normal start still requires two, explicit test accepts one");
        solo.tick(3000); finish_turn(solo,450);
        check(solo.state().phase==Phase::feedback && solo.state().target==450 && !solo.state().winner,"solo never immediately awards a win");
        next(solo); finish_turn(solo,450);
        check(solo.state().players[0].penalties==1 && solo.state().target==0,"solo tie earns the real first penalty");
        next(solo); finish_turn(solo,0); next(solo); finish_turn(solo,0);
        check(solo.state().phase==Phase::cancelled && solo.state().players[0].penalties==3 && valid_state(solo.state()),"three penalties finish solo testing");
        check(!partial.start_solo_test(1),"solo cannot replace a running multiplayer match");
        auto m = match(true); check(m.state().active == a, "host starts");
        const auto old = m.token(); finish_turn(m, 500.5); check(m.state().target == 500.5, "native score preserved without multiplier or rounding");
        next(m); check(m.state().active == b, "turn order"); finish_turn(m, 500.5);
        check(m.state().players[1].penalties == 1 && m.state().target == 0, "tie fails once and resets target");
        check(!m.trick_scored(old, 1, 99999, 100, m.turn_at()), "stale turn rejected");
        next(m); check(m.state().active == c, "next player sets after failure"); finish_turn(m, 1000);
        next(m); finish_turn(m, 1001); check(m.state().target == 1001, "higher score wins");
        auto zero = match(); finish_turn(zero, 0); check(zero.state().players[0].penalties == 1 && zero.state().target == 0, "opening zero earns one penalty");
        auto total=match(); auto t=total.token(); auto at=total.turn_at();
        check(total.trick_scored(t,1,200,100,at+100),"first trick counts before line finishes");
        check(total.trick_scored(t,2,350,200,at+200) && total.state().best==550,"all trick points add rather than selecting the best line");
        check(!total.trick_scored(t,2,350,200,at+201),"replayed trick rejected");
        check(!total.trick_scored(t,1,200,100,at+202),"stale trick rejected");
        check(!total.trick_scored(t,3,-5,300,at+300),"negative trick rejected");
        check(!total.trick_scored(t,3,std::numeric_limits<double>::infinity(),300,at+300),"infinite trick rejected");
        total.tick(at+30000); check(total.state().phase==Phase::settling && total.remaining(at+30000)==0,"clock stays at zero during network delivery");
        check(!total.trick_scored(t,3,1000,30000,at+30001),"exact deadline excluded, even inside unfinished combo");
        check(total.trick_scored(t,3,50,29999,at+30100),"pre-deadline trick may arrive slightly late");
        check(!total.trick_scored(t,4,1000,29999,at+31501),"delivery allowance bounded");
        total.tick(at+31500); check(total.state().target==600,"sum becomes next target without waiting for line end");
        check(!total.trick_scored(t,4,1000,100,at+31501),"judged turn cannot change");
        auto cutoff=match();at=cutoff.turn_at();cutoff.tick(at+31500);
        check(cutoff.state().players[0].penalties==1,"zero scores get one penalty");cutoff.tick(at+31501);
        check(cutoff.state().players[0].penalties==1,"resolution applied once");
        auto out = match(); for (unsigned i = 0; i < 3; ++i) { finish_turn(out, 0); if (i < 2) { next(out); finish_turn(out, 100); next(out); } }
        check(out.state().players[0].penalties == 3 && out.state().winner == b && out.state().phase == Phase::finished, "three penalties eliminate and choose winner");
        check(out.rematch(0) && !out.state().players[0].ready && out.state().players[0].penalties == 0, "rematch requires ready again");
        auto quit = match(true); check(quit.leave(a, 4000) && quit.state().phase == Phase::cancelled, "host departure cancels");
        quit = match(true); finish_turn(quit, 200); next(quit); check(quit.leave(b, quit.turn_at()) && quit.state().active == c && quit.state().target == 0, "active disconnect skips and resets");
        check(quit.leave(c, quit.turn_at()) && quit.state().winner == a, "disconnect winner");
        m = match(true); Message snap; snap.action = Action::snapshot; snap.match = m.state().match; snap.world = m.state().world; snap.leader = a; snap.state = m.state(); snap.remaining = 30000;
        const auto bytes = encode(snap); check(decode(bytes) == snap, "snapshot roundtrip");
        snap.facing={0,0,-1,0,1,0,1,0,0};
        check(decode(encode(snap))==snap,"selected flag facing reaches other players");
        auto bad_facing=snap;bad_facing.facing={};check(!valid_message(bad_facing),"collapsed flag rotation rejected");
        bad_facing=snap;bad_facing.facing[0]=std::numeric_limits<float>::quiet_NaN();check(!valid_message(bad_facing),"nonfinite flag rotation rejected");
        snap.state.config.max_players = 4;
        check(decode(encode(snap)) == snap, "selected player limit reaches other skaters");
        snap.state.config.max_players = 6;
        for (std::size_t i = 0; i < bytes.size(); ++i) check(!decode(std::span(bytes).first(i)), "truncated snapshot rejected");
        auto wrong = bytes; wrong[0] = 99; check(!decode(wrong), "incompatible version rejected"); wrong = bytes; wrong.push_back(0); check(!decode(wrong), "trailing garbage rejected");
        State guest; check(!apply_snapshot(guest, snap, b, 200), "only authenticated leader publishes"); check(apply_snapshot(guest, snap, a, 200), "guest adopts authoritative state");
        check(!apply_snapshot(guest, snap, a, 200), "duplicate snapshot rejected"); check(!apply_snapshot(guest, snap, a, 201), "map identity enforced");
        Message start; start.action = Action::trick_score; start.match = 100; start.world = 200; start.leader = a; start.sequence = 1; start.line = 1; start.turn = 1;
        auto end = start; end.action = Action::trick_score; end.sequence = 2; end.score = 500; end.flag = true;
        InputOrder order; check(order.push(end).empty(), "out of order end waits"); auto applied = order.push(start);
        check(applied.size() == 2 && applied[0] == start && applied[1] == end, "ordered replay"); check(order.push(end).empty(), "duplicate message ignored");
        end.sequence = 100; check(order.push(end).empty(), "bounded reorder window"); end.sequence = 3; end.score = std::numeric_limits<double>::quiet_NaN(); check(!valid_message(end), "nonfinite score rejected");
        check(valid_state(m.state()), "legal match snapshot");
        using namespace dingosdk::game::build::v20260929::one_up_scoring;
        check(graph_contract(start_graph, 0x40, 0x48, 0x7b), "actual line-start event contract");
        check(graph_contract(end_graph, 0x70, 0x2d0, 0x408), "telemetry finalized line event contract");
        check(graph_contract(stats_end_graph, 0x50, 0, 0x21), "stats finalized line event contract");
        check(!graph_contract(0x294b6b78, 0x40, 0xe8, 0xc6), "timer restarts cannot count as line starts");
        check(graph_contract(sequence_end_graph,0x470,0x908,0xffe),"landed trick telemetry contract");
        InputOrder resumed(8); auto resumed_input = start; resumed_input.sequence = 8;
        check(resumed.push(resumed_input).size() == 1 && resumed.push(resumed_input).empty(), "reconnected stream keeps duplicate protection");
        std::cout << checks << " 1-Up checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
