#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Skate 3 style game modes for a ReSkate session: Spot Jam, 1-Up, Hall of Meat, Deathrace,
// Domination, Graffiti and Skate Tag, played inside an area the player who starts the game marks out
// (or the whole map).
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

// infection: Skate Tag where the tagged stay infected and hunt too; whoever stays clean longest wins.
// hide: Hide & Seek. The seeker is blind while everyone hides (turn_s seconds), then hunts; whoever
// is found seeks too. Hiders score the time they stay hidden, seekers count who they find.
enum class Mode : std::uint8_t { jam = 1, one_up = 2, meat = 3, race = 4, domination = 5, graffiti = 6, tag = 7, skate = 8, infection = 9, hide = 10 };
inline constexpr Mode all_modes[]{Mode::jam,      Mode::one_up, Mode::meat,  Mode::race,     Mode::domination,
                                  Mode::graffiti, Mode::tag,    Mode::skate, Mode::infection, Mode::hide};
// Played on where everyone is (position events): Skate Tag, Infection and Hide & Seek.
inline constexpr bool tag_like(Mode m) noexcept { return m == Mode::tag || m == Mode::infection || m == Mode::hide; }
// The caught join the hunters: Infection and Hide & Seek.
inline constexpr bool hunt_like(Mode m) noexcept { return m == Mode::infection || m == Mode::hide; }
std::string_view mode_name(Mode) noexcept;    // "Spot Jam"
std::string_view mode_key(Mode) noexcept;     // "jam": what `mode new` takes
std::string_view mode_summary(Mode) noexcept; // one line on how it is played
std::string_view mode_tagline(Mode) noexcept; // a shorter line, for its card in skate.'s Throwdowns menu
// The order modes are offered in (Throwdowns cards): the newest and most played first.
inline constexpr Mode card_order[]{Mode::tag,  Mode::infection, Mode::hide,       Mode::skate,    Mode::race,
                                   Mode::meat, Mode::jam,       Mode::one_up,     Mode::domination, Mode::graffiti};
// A mode's place in card_order (past the end when it is not offered).
constexpr std::size_t card_rank(Mode mode) noexcept {
    for (std::size_t i = 0; i < std::size(card_order); ++i)
        if (card_order[i] == mode) return i;
    return std::size(card_order);
}
std::optional<Mode> parse_mode(std::string_view) noexcept;
bool timed(Mode) noexcept; // ends when the clock runs out (1-Up ends on strikes instead)

enum class Phase : std::uint8_t { setup = 1, countdown = 2, playing = 3, results = 4 };

inline constexpr std::uint8_t wire_magic = 0xD5; // never a throwdown message's first byte (1..15)
inline constexpr std::uint8_t wire_version = 10; // 2: circle areas; 3: Graffiti tags; 4: spawn, gate facings; 5: gate widths; 6: Skate Tag; 7: S.K.A.T.E.; 8: Infection; 9: Hide & Seek; 10: positions from the session's poses, not sent
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
    std::vector<float> widths;      // Deathrace: each gate's half width in metres (empty: `radius`)
    Vec3 spawn{};                   // where everyone starts when the countdown begins
    bool has_spawn{};
    std::uint8_t trick_kinds = 0x0f; // S.K.A.T.E.: the kinds of trick that may be set (trick_* bits)
    bool operator==(const Settings &) const = default;
};

// S.K.A.T.E.: the setter lands a trick, everyone else copies it or takes a letter; a setter who misses
// hands the set on. The tricks are skate.'s own names, as its score HUD lists them ("Kickflip",
// "Heelflip + Seatbelt", "BS 50-50 Grind"); a trick set once cannot be set again. The leader picks the
// kinds that may be set; a trick is of every kind any of its parts is.
inline constexpr std::uint8_t trick_flips = 1, trick_grabs = 2, trick_grinds = 4, trick_manuals = 8, all_trick_kinds = 0x0f;
inline constexpr std::size_t max_trick_length = 80;
std::uint8_t trick_kinds_of(std::string_view trick) noexcept;
bool trick_allowed(const Settings &, std::string_view trick) noexcept;
// "S.K.A" for three letters of S.K.A.T.E.; the letters spell as many as `strikes` allows.
std::string skate_letters(unsigned letters, unsigned of);
// What is missing before `mode start` (empty: ready).
std::string missing(const Settings &);

// Inside the area seen from above. Without an area (fewer than three corners) everywhere is.
bool inside(const std::vector<Vec3> &corners, const Vec3 &point) noexcept;
// The same for a game's area, a circle (area_radius) or the corners' shape.
bool inside(const Settings &, const Vec3 &point) noexcept;
bool has_area(const Settings &) noexcept;
// Where to put back a player who wandered out: the corners' average height, at their middle.
Vec3 area_centre(const std::vector<Vec3> &corners) noexcept;
inline constexpr float min_area_radius = 3.0f, max_area_radius = 1500.0f; // up to 3 km across (Skate Tag on a whole district)
// Skate Tag: within this far (metres, the game's adius) the one who is it tags; the one just tagged
// cannot tag back the one who tagged them for a while; a position older than this is not trusted.
inline constexpr float default_tag_reach = 3.0f;
inline constexpr std::uint64_t no_tag_back_ms = 3000, position_fresh_ms = 1500;
// A Deathrace gate's half width: its own, or the game's checkpoint radius.
inline constexpr float min_gate_half_width = 1.5f, max_gate_half_width = 40.0f;
float gate_half_width(const Settings &, std::size_t gate) noexcept;

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
// position: where the sender is (Skate Tag), sent a few times a second to every player.
// trick: a S.K.A.T.E. attempt, `trick` its name and value 1 landed, 0 missed (a bail, or nothing landed).
enum class Event : std::uint8_t { line = 1, bail = 2, checkpoint = 3, position = 4, trick = 5 };
struct Standing {
    std::uint64_t player{};
    std::int32_t score{}, aux{}; // aux: best line or bail, finish time (ms), strikes; Skate Tag: score the
                                 // tenths of a second spent it (fewer is better)
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
        tags = 7,  // leader: Graffiti tag shapes, from index `first` (state messages carry only their owners)
        hello = 8  // anyone with game modes, every few seconds: who has them, and (by the version byte) which
                   // (`leader` is the sender, `game` 1). A game of another version that cannot read it says so.
    };
    Kind kind = Kind::setup;
    std::uint8_t version = wire_version;
    std::uint64_t leader{};
    std::uint32_t game{};
    Settings settings;                         // setup
    Phase phase = Phase::setup;                // state
    std::uint32_t remaining_ms{};              // state: the countdown, clock or turn left
    std::uint64_t turn{};                      // state: 1-Up's player up; Skate Tag's player who is it
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
    std::string trick;                         // event: a S.K.A.T.E. attempt's trick; state: the trick to copy (empty: one to set)
    std::uint64_t setter{};                    // state: S.K.A.T.E.'s setter
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
               std::uint64_t now_ms, const std::vector<Tag> &tags = {}, std::string_view trick = {});
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
        Vec3 at{};                // Skate Tag: where they last said they were
        std::uint64_t at_time{};  // and when (0: never)
        std::uint64_t it_ms{};    // Skate Tag: time spent it; Infection: time survived
        bool infected{};          // Infection; Hide & Seek: seeking
    };
    Player *find(std::uint64_t id) noexcept;
    const Player *find(std::uint64_t id) const noexcept;
    std::string name_of(std::uint64_t id) const;
    void begin_play(std::uint64_t now_ms);
    void finish(std::uint64_t now_ms);
    void strike(Player &, std::uint64_t now_ms, std::string_view why);
    void next_turn(std::uint64_t now_ms);
    std::size_t still_in() const noexcept;
    // Skate Tag: player is it from now on.
    void make_it(std::uint64_t player, std::uint64_t by, std::uint64_t now_ms);
    void try_tags(std::uint64_t now_ms);
    // Infection: every infected player within reach of a survivor infects them.
    void try_infections(std::uint64_t now_ms);
    std::size_t survivors() const noexcept;
    // S.K.A.T.E.
    void skate_attempt(Player &, bool landed, std::string_view trick, std::uint64_t now_ms);
    void letter(Player &, std::string_view why);
    void pass_set(std::uint64_t now_ms);    // the setter missed: the next player still in sets
    void next_copier(std::uint64_t now_ms); // the next player to copy, or back to the setter
    std::size_t setter_{};
    std::string set_trick_;                 // the trick to copy; empty while the setter sets
    std::vector<std::string> done_tricks_;  // set already this game

    Settings settings_;
    std::uint64_t leader_{};
    std::uint32_t game_{};
    Phase phase_ = Phase::setup;
    std::uint64_t phase_at_{}, turn_at_{}, last_second_{};
    std::vector<Player> players_;
    std::size_t turn_{};
    std::int32_t target_{};
    std::uint64_t it_{}, tagged_by_{}, it_since_{}, it_counted_{}; // Skate Tag
    bool released_{};               // Hide & Seek: the hiding time is over
    std::uint64_t hide_end() const noexcept { return play_at_ + settings_.turn_s * 1000ull; }
    std::uint64_t play_at_{};
    std::vector<Tag> tags_;         // Graffiti
    std::vector<ZoneOwner> owners_; // one per Domination spot or Graffiti tag (owner 0: unclaimed)
    std::vector<std::string> calls_;   // not yet taken
    std::vector<std::string> recent_;  // the last few, for the state message
    std::uint32_t call_serial_{};
    void call(std::string text);
};
} // namespace dingosdk::modes
