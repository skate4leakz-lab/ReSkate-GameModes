// The game modes' rules and wire format without the game: referees are driven by hand with
// the events players would send.
#include "Extension/Modes/mode_rules.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace dingosdk::modes;

namespace {
void check(bool condition, const std::string &what) {
    if (!condition) throw std::runtime_error("FAILED: " + what);
}
constexpr std::uint64_t a = 76561198000000001ULL, b = 76561198000000002ULL, c = 76561198000000003ULL;
const std::vector<Vec3> square{{0, 0, 0}, {40, 0, 0}, {40, 0, 40}, {0, 0, 40}};

void area_flow() {
    check(inside(square, {20, 0, 20}), "the middle of a square is inside it");
    check(!inside(square, {50, 0, 20}), "a point past its edge is outside");
    check(inside(std::vector<Vec3>{}, {1e6f, 0, 1e6f}), "without an area everywhere counts");
    const auto centre = area_centre(square);
    check(centre[0] == 20 && centre[2] == 20, "the area's centre");
    Settings graffiti;
    graffiti.mode = Mode::graffiti;
    check(missing(graffiti).empty(), "Graffiti needs nothing set up: what gets skated is tagged");
    check(missing(Settings{Mode::race}) != "", "a race without checkpoints is not ready");
    Settings two_corners;
    two_corners.corners = {{0, 0, 0}, {1, 0, 1}};
    check(!missing(two_corners).empty(), "two corners are not an area");
    // A circle, like skate.'s own jam drops: a centre and how far it reaches.
    Settings circle;
    circle.mode = Mode::graffiti;
    circle.corners = {{100, 0, 100}};
    circle.area_radius = 20;
    check(has_area(circle) && missing(circle).empty(), "a centre and a radius are an area");
    check(inside(circle, {115, 0, 100}) && !inside(circle, {121, 0, 100}), "inside the circle and past it");
    Message setup;
    setup.kind = Message::Kind::setup;
    setup.leader = a;
    setup.game = 3;
    setup.settings = circle;
    check(decode(encode(setup)) == setup, "a circle round trips");
    circle.corners.push_back({0, 0, 0});
    setup.settings = circle;
    bool refused = false;
    try { (void)encode(setup); } catch (const std::invalid_argument &) { refused = true; }
    check(refused, "a circle has exactly one centre");
}

void scoring_flow() {
    check(trick_score({0.1f}) == 0, "a hop scores nothing");
    const auto ollie = trick_score({0.5f, 0.5f, 3});
    const auto kickflip = trick_score({0.5f, 0.5f, 3, 0, 0, 1000});
    const auto three_sixty = trick_score({0.8f, 1.0f, 4, 360});
    check(ollie > 0 && kickflip > ollie && three_sixty > kickflip, "flips and spins beat an ollie");
    check(line_score(1000, 1) == 1000 && line_score(1000, 3) == 2000, "a line multiplies its tricks");
    check(line_score(1000, 50) == 6000, "the multiplier is capped");
    check(bail_score({15, 4, 2}) > bail_score({5, 0, 1}), "a faster, higher bail is more meat");
}

void wire_flow() {
    Message setup;
    setup.kind = Message::Kind::setup;
    setup.leader = a;
    setup.game = 7;
    setup.settings.mode = Mode::domination;
    setup.settings.corners = square;
    setup.settings.points = {{10, 1, 10}, {30, 1, 30}};
    const auto bytes = encode(setup);
    check(is_mode_message(bytes) && bytes[0] != 25, "game mode messages are told apart by their first byte");
    check(decode(bytes) == setup, "setup round trip");
    Message state;
    state.kind = Message::Kind::state;
    state.leader = a;
    state.game = 7;
    state.phase = Phase::playing;
    state.remaining_ms = 1234;
    state.standings = {{a, 10, 2, false, true}, {b, 5, 0, true, false}};
    state.zones = {{1, b, 900}};
    state.calls = {"GO!", "Sam stole a tag from Alex"};
    state.call_serial = 12;
    check(decode(encode(state)) == state, "state round trip");
    Message join;
    join.kind = Message::Kind::join;
    join.leader = a;
    join.game = 7;
    check(decode(encode(join)) == join, "join round trip");
    Message event;
    event.kind = Message::Kind::event;
    event.leader = a;
    event.game = 7;
    event.value = 4000;
    event.extra = 3;
    event.at = {1, 2, 3};
    event.sequence = 9;
    check(decode(encode(event)) == event, "event round trip");
    auto future = encode(event);
    future[1] = wire_version + 1;
    check(!decode(future) && message_version(future) == wire_version + 1, "another version is recognised, not read");
    auto cut = encode(state);
    cut.pop_back();
    check(!decode(cut), "a cut message is rejected");
    check(!decode(std::vector<std::uint8_t>{1, 2, 3}), "a throwdown message is not a game mode message");
}

void jam_flow() {
    Settings s;
    s.mode = Mode::jam;
    s.duration_s = 60;
    s.corners = square;
    Referee r(s, a, 1);
    r.add_player(a);
    r.add_player(b);
    r.start(0);
    r.event(a, Event::line, 500, {10, 0, 10}, 1, 1000);
    check(r.state(1000).standings[0].score == 0, "nothing counts during the countdown");
    check(r.tick(countdown_ms), "the countdown ends");
    r.event(a, Event::line, 500, {10, 0, 10}, 2, 6000);
    r.event(a, Event::line, 500, {10, 0, 10}, 2, 6100); // a repeat
    r.event(b, Event::line, 900, {10, 0, 10}, 1, 7000);
    r.event(b, Event::line, 5000, {99, 0, 99}, 2, 7500); // outside the area
    auto st = r.state(8000);
    check(st.standings[0].player == b && st.standings[0].score == 900, "b leads with one line");
    check(st.standings[1].score == 500, "a's repeated event counted once");
    r.tick(countdown_ms + 60000);
    check(r.phase() == Phase::results, "the clock runs out");
    check(r.finished(countdown_ms + 60000 + results_ms), "results show, then the game is over");
}

void one_up_flow() {
    Settings s;
    s.mode = Mode::one_up;
    s.strikes = 2;
    Referee r(s, a, 2);
    r.add_player(a);
    r.add_player(b);
    r.start(0);
    r.tick(countdown_ms);
    auto st = r.state(countdown_ms);
    check(st.turn == a, "the leader goes first");
    r.event(b, Event::line, 9999, {}, 1, countdown_ms + 10); // not b's turn
    check(r.state(countdown_ms + 10).turn == a, "only the player up counts");
    r.event(a, Event::line, 1000, {}, 1, countdown_ms + 100);
    st = r.state(countdown_ms + 100);
    check(st.turn == b && st.target == 1000, "a set 1000, b is up");
    r.event(b, Event::line, 800, {}, 2, countdown_ms + 200);
    st = r.state(countdown_ms + 200);
    check(st.turn == a, "b fell short: a strike, a is up");
    r.event(a, Event::bail, 100, {}, 2, countdown_ms + 300);
    st = r.state(countdown_ms + 300);
    check(r.phase() == Phase::playing && st.turn == b, "a bailed: one strike each, b is up");
    r.tick(countdown_ms + 300 + s.turn_s * 1000ull);
    check(r.phase() == Phase::results, "b ran out of time: two strikes and out, the last one standing wins");
    check(r.state(countdown_ms + 300 + s.turn_s * 1000ull).standings[0].player == a, "a wins");
}

void race_flow() {
    Settings s;
    s.mode = Mode::race;
    s.points = {{0, 0, 0}, {50, 0, 0}, {100, 0, 0}};
    Referee r(s, a, 3);
    r.add_player(a);
    r.add_player(b);
    r.start(0);
    r.tick(countdown_ms);
    r.event(a, Event::checkpoint, 1, {50, 0, 0}, 1, countdown_ms + 100);
    check(r.state(countdown_ms + 100).standings[0].score == 0, "checkpoints go in order");
    r.event(a, Event::checkpoint, 0, {0, 0, 0}, 2, countdown_ms + 200);
    r.event(a, Event::checkpoint, 1, {50, 0, 0}, 3, countdown_ms + 300);
    r.event(a, Event::checkpoint, 2, {100, 0, 0}, 4, countdown_ms + 400);
    r.event(b, Event::checkpoint, 0, {0, 0, 0}, 1, countdown_ms + 500);
    r.event(b, Event::checkpoint, 1, {500, 0, 0}, 2, countdown_ms + 600); // nowhere near it
    auto st = r.state(countdown_ms + 600);
    check(st.standings[0].player == a && st.standings[0].aux == 400, "a finished in 0.4 s");
    check(st.standings[1].score == 1, "b's far-off checkpoint was refused");
    r.remove_player(b, countdown_ms + 700);
    r.tick(countdown_ms + 700);
    check(r.phase() == Phase::results, "everyone still racing finished");
}

void domination_flow() {
    Settings s;
    s.mode = Mode::domination;
    s.points = {{0, 0, 0}, {100, 0, 0}};
    Referee r(s, a, 4);
    r.add_player(a);
    r.add_player(b);
    r.start(0);
    r.tick(countdown_ms);
    r.event(a, Event::line, 500, {1, 0, 1}, 1, countdown_ms);
    r.tick(countdown_ms + 3000);
    check(r.state(countdown_ms + 3000).standings[0].score == 3, "a spot held for 3 s is 3 points");
    r.event(b, Event::line, 400, {1, 0, 1}, 1, countdown_ms + 3000);
    check(r.state(countdown_ms + 3000).zones[0].owner == a, "a smaller line does not take it");
    r.event(b, Event::line, 600, {1, 0, 1}, 2, countdown_ms + 3000);
    check(r.state(countdown_ms + 3000).zones[0].owner == b, "a bigger line does");
}

void tag_flow() {
    // A curb ground twice from different ends is one curb; a ledge beside it is another.
    const Tag curb{TagKind::grind, {{0, 0.2f, 0}, {2, 0.2f, 0}, {4, 0.2f, 0}}};
    const Tag same_curb{TagKind::grind, {{3, 0.25f, 0.3f}, {5, 0.2f, 0.2f}, {6, 0.2f, 0}}};
    const Tag ledge{TagKind::grind, {{0, 0.5f, 5}, {4, 0.5f, 5}}};
    check(same_tag(curb, same_curb) && !same_tag(curb, ledge), "grinds on one curb are the same tag");
    const Tag stairs{TagKind::gap, {{10, 2, 10}, {13, 0, 10}}}, stairs_again{TagKind::gap, {{10.5f, 2, 9}, {14, 0, 11}}};
    check(same_tag(stairs, stairs_again) && !same_tag(stairs, curb), "a gap jumped again is the same gap");
    std::vector<Vec3> long_path;
    for (int i = 0; i <= 30; ++i) long_path.push_back({static_cast<float>(i), 0, 0});
    const auto thin = thin_path(long_path);
    check(thin.size() == max_tag_points && thin.front()[0] == 0 && thin.back()[0] == 30, "a long grind keeps its ends");
    Message event;
    event.kind = Message::Kind::event;
    event.leader = a;
    event.game = 4;
    event.value = 900;
    event.sequence = 1;
    event.tags = {curb, stairs};
    check(decode(encode(event)) == event, "a line's tags round trip");
    Message shapes;
    shapes.kind = Message::Kind::tags;
    shapes.leader = a;
    shapes.game = 4;
    shapes.first = 3;
    shapes.tags = {ledge};
    check(decode(encode(shapes)) == shapes, "tag shapes round trip");
}

void graffiti_flow() {
    Settings s;
    s.mode = Mode::graffiti;
    s.corners = square;
    Referee r(s, a, 5);
    r.add_player(a);
    r.add_player(b);
    r.add_player(c);
    r.start(0);
    r.tick(countdown_ms);
    const Tag curb{TagKind::grind, {{5, 0.2f, 5}, {10, 0.2f, 5}}};
    const Tag rail{TagKind::grind, {{30, 0.8f, 30}, {35, 0.8f, 30}}};
    const Tag outside{TagKind::grind, {{90, 0.2f, 90}, {95, 0.2f, 90}}};
    r.event(a, Event::line, 500, {10, 0, 5}, 1, countdown_ms, {curb});
    r.event(a, Event::line, 500, {35, 0, 30}, 2, countdown_ms, {rail, outside});
    check(r.tags().size() == 2, "two tags; the one outside the area is not");
    r.event(b, Event::line, 400, {10, 0, 5}, 1, countdown_ms, {{TagKind::grind, {{6, 0.2f, 5}, {9, 0.2f, 5}}}});
    check(r.state(countdown_ms).zones[0].owner == a, "a smaller line does not steal the curb");
    r.event(b, Event::line, 800, {10, 0, 5}, 2, countdown_ms, {{TagKind::grind, {{6, 0.2f, 5}, {9, 0.2f, 5}}}});
    auto st = r.state(countdown_ms);
    check(r.tags().size() == 2 && st.zones.size() == 2, "the same curb, still two tags");
    check(st.standings[0].score == 1 && st.standings[1].score == 1, "b stole one of a's");
    const auto calls = r.take_calls();
    check(!calls.empty() && calls.back().find("stole") != std::string::npos, "the steal is called out");
    const auto shown = r.state(countdown_ms);
    check(shown.calls.size() <= max_calls && shown.calls.back() == calls.back(), "the state carries the latest calls");
    check(decode(encode(shown)) == shown, "a real state round trips");
    check(r.state(countdown_ms).standings.size() == 3, "everyone is on the board");
}
} // namespace

int main() {
    try {
        area_flow();
        scoring_flow();
        wire_flow();
        jam_flow();
        one_up_flow();
        race_flow();
        domination_flow();
        tag_flow();
        graffiti_flow();
        std::cout << "Game modes: area, scoring, wire, Spot Jam, 1-Up, Deathrace, Domination, tag and Graffiti flows passed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
