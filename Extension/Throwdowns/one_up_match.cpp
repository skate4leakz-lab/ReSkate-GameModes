#include "one_up_match.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <set>

namespace dingosdk::multiplayer::one_up {
bool Config::valid() const noexcept {
    return turn_ms >= 10000 && turn_ms <= 120000 && grace_ms == 0 &&
        countdown_ms >= 1000 && countdown_ms <= 10000 && delivery_ms <= 3000 && feedback_ms <= 10000 &&
        max_players >= 1 && max_players <= 6;
}
Match::Match(std::uint64_t identity, std::uint64_t world, PlayerId leader, Config config) {
    if (!identity || !world || !leader || !config.valid()) throw std::invalid_argument("Invalid 1-Up match");
    state_.match = identity; state_.world = world; state_.leader = leader; state_.config = config;
    state_.players.push_back({leader}); state_.notice = "Choose Ready when you are at the spot."; changed();
}
void Match::changed() { ++state_.revision; }
Player* Match::player(PlayerId id) {
    const auto it = std::find_if(state_.players.begin(), state_.players.end(), [id](const Player& p) { return p.id == id; });
    return it == state_.players.end() ? nullptr : &*it;
}
bool Match::join(PlayerId id) {
    if (state_.phase != Phase::lobby || !id || player(id) || state_.players.size() >= state_.config.max_players) return false;
    state_.players.push_back({id});
    // The host leads, then Steam IDs: crossing join messages do not change turn order.
    std::sort(state_.players.begin() + 1, state_.players.end(), [](const Player& a, const Player& b) { return a.id < b.id; });
    changed(); return true;
}
bool Match::ready(PlayerId id, bool value) {
    auto* p = player(id);
    if (state_.phase != Phase::lobby || !p || !p->connected || p->ready == value) return false;
    p->ready = value; changed(); return true;
}
bool Match::start(Time now) {
    if (state_.config.max_players == 1) return start_solo_test(now);
    if (state_.phase != Phase::lobby || state_.players.size() < 2 ||
        std::any_of(state_.players.begin(), state_.players.end(), [](const Player& p) { return !p.connected || !p.ready; })) return false;
    cursor_ = 0; state_.target = 0; state_.winner = 0;
    countdown(now); return true;
}
bool Match::start_solo_test(Time now) {
    if(state_.phase!=Phase::lobby || state_.players.size()!=1 ||
       !state_.players.front().connected || !state_.players.front().ready) return false;
    solo_test_=true; cursor_=0; state_.target=0; state_.winner=0;
    countdown(now); return true;
}
bool Match::position_confirmed(Token token_value, Time now) {
    if (state_.phase != Phase::countdown || !(token_value == token()) || now < phase_at_) return false;
    phase_at_ = now; changed(); return true;
}
bool Match::rematch(Time) {
    if (state_.phase != Phase::finished) return false;
    std::erase_if(state_.players, [](const Player& p) { return !p.connected; });
    for (auto& p : state_.players) { p.penalties = 0; p.ready = false; }
    state_.phase = Phase::lobby; state_.target = state_.best = 0; state_.winner = state_.active = 0;
    state_.judged_player = 0; state_.notice = "Rematch: choose Ready."; changed(); return true;
}
void Match::cancel(std::string reason) {
    if (state_.phase == Phase::cancelled) return;
    state_.phase = Phase::cancelled; state_.active = state_.winner = 0; state_.notice = std::move(reason); changed();
}
bool Match::winner() {
    std::size_t live{}; PlayerId last{};
    for (const auto& p : state_.players) if (p.eligible()) { ++live; last = p.id; }
    // Practice uses real local turns and real scoring, with no fabricated rival.
    // Keep playing against the previous target until all three penalties accrue.
    if(solo_test_) {
        if(live) return false;
        cancel("Solo test complete: 1UP. Place a new flag to try again."); return true;
    }
    if (live > 1) return false;
    state_.active = 0; state_.winner = last;
    state_.phase = live ? Phase::finished : Phase::cancelled;
    state_.notice = live ? "Last skater standing!" : "Everyone left the match."; changed(); return true;
}
bool Match::leave(PlayerId id, Time now) {
    auto* p = player(id);
    if (!p || !p->connected) return false;
    if (id == state_.leader) { cancel("The host left. Create a new match to play again."); return true; }
    if (state_.phase == Phase::lobby) { std::erase_if(state_.players, [id](const Player& q) { return q.id == id; }); changed(); return true; }
    p->connected = false; changed();
    if (state_.phase == Phase::finished || state_.phase == Phase::cancelled) return true;
    if (winner()) return true;
    if (state_.active == id) {
        state_.target = state_.best = 0; state_.notice = "The active skater left. Set a fresh target.";
        advance(now);
    }
    return true;
}
void Match::countdown(Time now) {
    state_.active = state_.players[cursor_].id;
    if (state_.turn == UINT32_MAX) { cancel("Match turn limit reached."); return; }
    ++state_.turn; state_.best = 0; last_trick_ = 0;
    state_.phase = Phase::countdown; phase_at_ = now;
    state_.notice = state_.target > 0 ? "Beat the target! Every landed trick counts." : "Build your total before time runs out!"; changed();
}
bool Match::accepts(Token t) const noexcept {
    return t == token() && (state_.phase == Phase::playing || state_.phase == Phase::settling);
}
bool Match::trick_scored(Token t,std::uint64_t id,double points,std::uint32_t elapsed,Time received) {
    if(!accepts(t) || !id || id<=last_trick_ || !std::isfinite(points) || points<=0 ||
       points>2147483647.0-state_.best || elapsed>=state_.config.turn_ms || received<turn_at_ ||
       received-turn_at_>state_.config.turn_ms+state_.config.delivery_ms)return false;
    last_trick_=id; state_.best+=points; changed(); return true;
}
void Match::resolve(Time now) {
    // This transition is called once. Inputs cannot mutate feedback/finished states.
    auto* p = player(state_.active);
    if (!p) { cancel("The active skater is unavailable."); return; }
    const bool success = state_.best > state_.target && state_.best > 0;
    state_.judged_player = p->id; state_.judged_score = state_.best; state_.judged_success = success;
    if (success) { state_.target = state_.best; state_.notice = "Target beaten!"; }
    else {
        ++p->penalties; state_.target = 0;
        state_.notice = p->penalties == 3 ? "1UP - eliminated!" : "Penalty earned. The next skater sets a fresh target.";
    }
    state_.phase = Phase::feedback; phase_at_ = now; changed();
    winner();
}
void Match::advance(Time now) {
    if (winner()) return;
    for (std::size_t n = 0; n < state_.players.size(); ++n) {
        cursor_ = (cursor_ + 1) % state_.players.size();
        if (state_.players[cursor_].eligible()) { countdown(now); return; }
    }
    cancel("No skaters remain.");
}
void Match::tick(Time now) {
    if (now < phase_at_) return;
    if (state_.phase == Phase::countdown && now - phase_at_ >= state_.config.countdown_ms) {
        state_.phase = Phase::playing; turn_at_ = now; phase_at_ = now; changed();
    }
    if (state_.phase == Phase::playing && now >= turn_at_ && now - turn_at_ >= state_.config.turn_ms) {
        state_.phase = Phase::settling; phase_at_ = turn_at_ + state_.config.turn_ms; changed();
    }
    if (state_.phase == Phase::settling) {
        // Delivery allowance accepts only events stamped BEFORE zero. It never
        // extends skating time or waits for a combo/multiplier to finish.
        if(now>=turn_at_+state_.config.turn_ms+state_.config.delivery_ms) resolve(now);
    }
    if (state_.phase == Phase::feedback && now - phase_at_ >= state_.config.feedback_ms) advance(now);
}
std::uint32_t Match::remaining(Time now) const noexcept {
    Time end{};
    if (state_.phase == Phase::countdown) end = phase_at_ + state_.config.countdown_ms;
    if (state_.phase == Phase::playing) end = turn_at_ + state_.config.turn_ms;
    return now < end ? static_cast<std::uint32_t>(end - now) : 0;
}
bool valid_state(const State& s) noexcept {
    if (!s.match || !s.world || !s.revision || !s.leader || !s.config.valid() || s.players.empty() || s.players.size() > s.config.max_players ||
        s.notice.size() > 192 || static_cast<unsigned>(s.phase) > static_cast<unsigned>(Phase::cancelled) ||
        s.players.front().id != s.leader || !std::isfinite(s.target) || !std::isfinite(s.best) || !std::isfinite(s.judged_score) ||
        s.target < 0 || s.best < 0 || s.judged_score < 0 || s.target > 2147483647.0 || s.best > 2147483647.0 || s.judged_score > 2147483647.0) return false;
    std::set<PlayerId> ids;
    for (const auto& p : s.players) if (!p.id || p.penalties > 3 || !ids.insert(p.id).second) return false;
    const auto eligible = [&](PlayerId id) { return std::any_of(s.players.begin(), s.players.end(), [id](const Player& p) { return p.id == id && p.eligible(); }); };
    if (s.phase == Phase::countdown || s.phase == Phase::playing || s.phase == Phase::settling)
        if (!s.turn || !eligible(s.active)) return false;
    if (s.phase == Phase::finished && (!eligible(s.winner) || s.active || std::count_if(s.players.begin(), s.players.end(), [](const Player& p) { return p.eligible(); }) != 1)) return false;
    return !s.judged_player || ids.contains(s.judged_player);
}
} // namespace dingosdk::multiplayer::one_up
