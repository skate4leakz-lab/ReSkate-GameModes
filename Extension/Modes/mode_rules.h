#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Skate 3 style game modes for a ReSkate session: Spot Jam, 1-Up, Hall of Meat, Deathrace,
// Domination and Graffiti, played inside an area the player who starts the game marks out.
//
// The player who starts a game is its leader and referees it: the settings, the clock and
// the standings are theirs. Every player (the leader too) reports only their own lines,
// bails and checkpoints; the leader applies them and sends the state everyone draws.
// Messages ride the throwdown channel (relayed as-is by hosts and dedicated servers), told
// apart by their first byte, so players without game modes drop them like any message
// they cannot read and the session protocol stays the one everyone else speaks.
//
// Nothing here touches the game: plain rules and the wire format, unit-tested.
namespace dingosdk::modes {
using Vec3 = std::array<float, 3>;

enum class Mode : std::uint8_t { jam = 1, one_up = 2, meat = 3, race = 4, domination = 5, graffiti = 6 };
inline constexpr Mode all_modes[]{Mode::jam, Mode::one_up, Mode::meat, Mode::race, Mode::domination, Mode::graffiti};
std::string_view mode_name(Mode) noexcept;    // "Spot Jam"
std::string_view mode_key(Mode) noexcept;     // "jam": what `mode new` takes
std::string_view mode_summary(Mode) noexcept; // one line on how it is played
std::optional<Mode> parse_mode(std::string_view) noexcept;
bool timed(Mode) noexcept; // ends when the clock runs out (1-Up ends on strikes instead)

enum class Phase : std::uint8_t { setup = 1, countdown = 2, playing = 3, results = 4 };

inline constexpr std::uint8_t wire_magic = 0xD5; // never a throwdown message's first byte (1..15)
inline constexpr std::uint8_t wire_version = 4; // 2: circle areas; 3: Graffiti tags; 4: spawn, gate facings
inline constexpr std::size_t max_corners = 16, max_points = 16, max_players = 16, max_zones = 64, max_calls = 4,
                             max_call_length = 96, max_tags = 64, max_tag_points = 6, max_line_tags = 6;
inline constexpr std::uint32_t countdown_ms = 5000, results_ms = 12000;

struct Settings {
    Mode mode = Mode::jam;
    std::uint32_t duration_s = 300; // timed modes
    std::uint32_t turn_s = 30;      // 1-Up: how long each turn is
    std::uint8_t strikes = 3;       // 1-Up: misses before a player is out
    float radius = 6.0f;            // a checkpoint's or spot's reach (metres)
    std::vector<Vec3> corners;      // the area, in order around it (x and z; y is the ground there)
    float area_radius{};            // > 0: the area is a circle this wide around corners[0] instead
    std::vector<Vec3> points;       // Deathrace checkpoints in order, Domination spots
    std::vector<float> yaws;        // Deathrace: each gate's facing in degrees (empty: along the route)
    Vec3 spawn{};                   // where everyone starts when the countdown begins
    bool has_spawn{};
    bool operator==(const Settings &) const = default;
};
// What is missing before `mode start` (empty: ready).
std::string missing(const Settings &);

// Inside the area seen from above. Without an area (fewer than three corners) everywhere is.
bool inside(const std::vector<Vec3> &corners, const Vec3 &point) noexcept;
// The same for a game's area, a circle (area_radius) or the corners' shape.
bool inside(const Settings &, const Vec3 &point) noexcept;
bool has_area(const Settings &) noexcept;
// Where to put back a player who wandered out: the corners' average height, at their middle.
Vec3 area_centre(const std::vector<Vec3> &corners) noexcept;
inline constexpr float min_area_radius = 3.0f, max_area_radius = 300.0f;

// Graffiti tags, the way THPS tags what you skate: a grind is the path the board slid along a
// curb, ledge or rail; a gap is a drop of a metre or more from takeoff to landing (stair sets,
// gaps). A tag on the same thing as an existing one is that tag again, so a bigger line steals it.
enum class TagKind : std::uint8_t { grind = 1, gap = 2 };
struct Tag {
    TagKind kind = TagKind::grind;
    std::vector<Vec3> path; // grind: along the object (2..max_tag_points); gap: takeoff, landing
    bool operator==(const Tag &) const = default;
};
bool same_tag(const Tag &, const Tag &) noexcept;
Vec3 tag_centre(const Tag &) noexcept;
// At most `count` points spread evenly along a path, its ends kept.
std::vector<Vec3> thin_path(const std::vector<Vec3> &path, std::size_t count = max_tag_points);
inline constexpr float gap_drop = 0.9f; // metres a landing must be below its takeoff to tag a gap
// The Domination spot a point is on (nearest within reach).
std::optional<std::size_t> spot_at(const Settings &, const Vec3 &) noexcept;

// Scoring, from what the Trainer measures of each jump. The game's own trick score is only
// handed out inside its throwdowns, so ReSkate scores tricks itself: big air, big spins,
// flips and fast board rotation all count, and a line multiplies its tricks' sum.
struct JumpSample {
    float air_time{}, height{}, distance{}, spin{}, flip{}, board_turn{};
};
std::int32_t trick_score(const JumpSample &) noexcept;
std::int32_t line_score(std::int64_t sum, unsigned tricks) noexcept;
struct BailSample {
    float speed{};  // m/s over the ground when it began
    float drop{};   // metres fallen while bailing
    float tumble{}; // seconds spent wiped out
};
std::int32_t bail_score(const BailSample &) noexcept;

// ---- wire
enum class Event : std::uint8_t { line = 1, bail = 2, checkpoint = 3 };
struct Standing {
    std::uint64_t player{};
    std::int32_t score{}, aux{}; // aux: best line or bail, finish time (ms), strikes
    bool out{}, up{};
    bool operator==(const Standing &) const = default;
};
struct ZoneOwner {
    std::uint8_t zone{};
    std::uint64_t owner{};
    std::int32_t best{};
    bool operator==(const ZoneOwner &) const = default;
};
struct Message {
    enum class Kind : std::uint8_t {
        setup = 1, // leader: the game and its settings (sent again every few seconds for late arrivals)
        state = 2, // leader: phase, clock, turn, standings, zones
        event = 3, // a player: one of their own lines, bails or checkpoints
        leave = 4, // a player: out of the game
        end = 5,   // leader: the game is over or cancelled
        join = 6,  // a player with game modes saw the setup and plays (only they are added)
        tags = 7   // leader: Graffiti tag shapes, from index `first` (state messages carry only their owners)
    };
    Kind kind = Kind::setup;
    std::uint8_t version = wire_version;
    std::uint64_t leader{};
    std::uint32_t game{};
    Settings settings;                         // setup
    Phase phase = Phase::setup;                // state
    std::uint32_t remaining_ms{};              // state: the countdown, clock or turn left
    std::uint64_t turn{};                      // state: 1-Up's player up
    std::int32_t target{};                     // state: 1-Up's score to beat
    std::vector<Standing> standings;           // state, best first
    std::vector<ZoneOwner> zones;              // state: held Domination spots or Graffiti tags (by index)
    // state: the referee's latest callouts ("Sam stole a tag"), oldest first; the last one is
    // number `call_serial`, so a player shows only those newer than the ones it has seen.
    std::vector<std::string> calls;
    std::uint32_t call_serial{};
    Event event = Event::line;                 // event
    std::int32_t value{}, extra{};             // event: score (line, bail) or checkpoint index; tricks in the line
    Vec3 at{};                                 // event: where it happened
    std::uint32_t sequence{};                  // event: the sender's own count (repeats are dropped)
    std::vector<Tag> tags;                     // event: what a Graffiti line was skated on; tags: the shapes
    std::uint8_t first{};                      // tags: the index of the first shape
    bool operator==(const Message &) const = default;
};
inline bool is_mode_message(std::span<const std::uint8_t> bytes) noexcept {
    return !bytes.empty() && bytes[0] == wire_magic;
}
// Throws std::invalid_argument for a message that breaks the limits above.
std::vector<std::uint8_t> encode(const Message &);
// Nothing for bytes that are not a game mode message of this version.
std::optional<Message> decode(std::span<const std::uint8_t>) noexcept;
// The version a game mode message was written with (0: not one), for an "update" notice.
std::uint8_t message_version(std::span<const std::uint8_t>) noexcept;

// ---- the leader's referee
class Referee {
  public:
    Referee(Settings settings, std::uint64_t leader, std::uint32_t game);
    const Settings &settings() const noexcept { return settings_; }
    Phase phase() const noexcept { return phase_; }
    std::uint32_t game() const noexcept { return game_; }
    // Players can be added until the countdown ends; 1-Up turns go in the order added.
    void add_player(std::uint64_t player);
    void remove_player(std::uint64_t player, std::uint64_t now_ms);
    bool has_player(std::uint64_t player) const noexcept;
    void start(std::uint64_t now_ms);
    // The leader ends the game early: straight to the results, standings as they are.
    void end_now(std::uint64_t now_ms) { if (phase_ == Phase::playing || phase_ == Phase::countdown) finish(now_ms); }
    void event(std::uint64_t player, Event event, std::int32_t value, const Vec3 &at, std::uint32_t sequence,
               std::uint64_t now_ms, const std::vector<Tag> &tags = {});
    const std::vector<Tag> &tags() const noexcept { return tags_; }
    // Advances the clock; true when the state changed in a way worth sending at once.
    bool tick(std::uint64_t now_ms);
    bool finished(std::uint64_t now_ms) const noexcept; // results shown long enough
    Message state(std::uint64_t now_ms) const;
    // Short lines for everyone's HUD since the last call ("Sam took zone 4").
    std::vector<std::string> take_calls();
    // Names for the calls; unknown ids read "A player".
    void set_name(std::uint64_t player, std::string name);

  private:
    struct Player {
        std::uint64_t id{};
        std::int32_t score{}, aux{};
        std::uint8_t strikes{};
        bool out{}, finished{};
        std::uint32_t sequence{};
        std::string name;
    };
    Player *find(std::uint64_t id) noexcept;
    const Player *find(std::uint64_t id) const noexcept;
    std::string name_of(std::uint64_t id) const;
    void begin_play(std::uint64_t now_ms);
    void finish(std::uint64_t now_ms);
    void strike(Player &, std::uint64_t now_ms, std::string_view why);
    void next_turn(std::uint64_t now_ms);
    std::size_t still_in() const noexcept;

    Settings settings_;
    std::uint64_t leader_{};
    std::uint32_t game_{};
    Phase phase_ = Phase::setup;
    std::uint64_t phase_at_{}, turn_at_{}, last_second_{};
    std::vector<Player> players_;
    std::size_t turn_{};
    std::int32_t target_{};
    std::vector<Tag> tags_;         // Graffiti
    std::vector<ZoneOwner> owners_; // one per Domination spot or Graffiti tag (owner 0: unclaimed)
    std::vector<std::string> calls_;   // not yet taken
    std::vector<std::string> recent_;  // the last few, for the state message
    std::uint32_t call_serial_{};
    void call(std::string text);
};
} // namespace dingosdk::modes
