#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dingosdk::multiplayer::one_up {
using PlayerId = std::uint64_t;
using Time = std::uint64_t; // monotonic milliseconds, owned by the match leader
struct Config {
    std::uint32_t turn_ms = 20000, grace_ms = 0, countdown_ms = 3000;
    // Reliable score delivery may finish after the visible clock. This is not skating time.
    std::uint32_t delivery_ms = 1500, feedback_ms = 2500;
    std::uint32_t max_players = 6;
    bool valid() const noexcept;
    bool operator==(const Config&) const = default;
};
enum class Phase : std::uint8_t { lobby, countdown, playing, settling, feedback, finished, cancelled };
struct Player {
    PlayerId id{};
    std::uint8_t penalties{};
    bool ready{}, connected = true;
    bool eligible() const noexcept { return connected && penalties < 3; }
    bool operator==(const Player&) const = default;
};
// Every native scoring event carries the turn that was active when it occurred.
struct Token {
    std::uint64_t match{};
    std::uint32_t turn{};
    PlayerId player{};
    bool operator==(const Token&) const = default;
};
struct State {
    std::uint64_t match{}, world{}, revision{};
    PlayerId leader{}, active{}, winner{}, judged_player{};
    Config config;
    Phase phase = Phase::lobby;
    std::uint32_t turn{};
    double target{}, best{}, judged_score{};
    bool judged_success{};
    std::vector<Player> players;
    std::string notice;
    bool operator==(const State&) const = default;
};
// No engine calls or transport here. Only the leader owns a Match. Everyone else consumes
// validated snapshots; local clocks never award penalties or advance remote turns.
class Match {
public:
    Match(std::uint64_t identity, std::uint64_t world, PlayerId leader, Config config = {});
    const State& state() const noexcept { return state_; }
    Time phase_at() const noexcept { return phase_at_; }
    Time turn_at() const noexcept { return turn_at_; }
    Token token() const noexcept { return {state_.match, state_.turn, state_.active}; }
    bool join(PlayerId);
    bool ready(PlayerId, bool);
    bool start(Time);
    bool start_solo_test(Time);
    bool solo_test() const noexcept { return solo_test_; }
    bool position_confirmed(Token, Time);
    bool rematch(Time);
    bool leave(PlayerId, Time);
    void cancel(std::string reason);
    // elapsed is measured from the owner's received turn start, not from wall clock time.
    // A landed trick/sequence's final native points, accepted before the visible
    // turn deadline. Earlier points stay banked if a later trick is bailed.
    bool trick_scored(Token, std::uint64_t event, double native_score, std::uint32_t elapsed, Time received);
    void tick(Time);
    std::uint32_t remaining(Time) const noexcept;
private:
    State state_;
    Time phase_at_{}, turn_at_{};
    std::uint64_t last_trick_{};
    bool solo_test_{};
    std::size_t cursor_{};
    Player* player(PlayerId);
    bool accepts(Token) const noexcept;
    void changed();
    bool winner();
    void countdown(Time);
    void resolve(Time);
    void advance(Time);
};
bool valid_state(const State&) noexcept;
} // namespace dingosdk::multiplayer::one_up
