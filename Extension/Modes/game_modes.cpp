#include "game_modes.h"
#include "bone_cam.h"
#include "mode_rules.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/HallOfMeat/hall_of_meat.h"
#include "Extension/HallOfMeat/hall_of_meat_hud.h"
#include "Engine/Core/Platform/memory.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "trick_gestures.h"
#include "Extension/Throwdowns/one_up_placement.h"
#include "Engine/Game/UI/game_view.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Console/commands.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <algorithm>
#include <charconv>
#include <deque>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace dingosdk::modes {
namespace {
constexpr std::uint32_t wipeout_state = 300, offboard_states_first = 500;
constexpr std::uint64_t line_grace_ms = 2500, bail_settle_ms = 3500, leader_timeout_ms = 10000, setup_every_ms = 3000,
                        state_every_ms = 500, join_every_ms = 2000, out_of_area_ms = 3000, popup_ms = 2500;

// The line the local skater is riding: tricks landed one after another, none bailed.
struct Line {
    unsigned tricks{};
    std::int64_t sum{};
    std::uint64_t landed{};
    Vec3 at{};
    std::vector<Tag> tags;   // Graffiti: what the line was skated on
    std::vector<Vec3> grind; // the grind under way: where the board has slid so far
    std::uint64_t grind_started{};
};
// Grinds and slides, from the physics states recorded on 2026-10-08 (402/403 grinds, 404 slides).
constexpr std::uint32_t grind_states_first = 400, grind_states_end = 500;
std::int32_t grind_score(float seconds, float metres) {
    return static_cast<std::int32_t>(150.0f + std::clamp(seconds, 0.0f, 10.0f) * 250.0f + std::clamp(metres, 0.0f, 60.0f) * 30.0f);
}
// A bail being measured until the skater settles.
struct Bail {
    bool active{};
    std::uint64_t started{};
    float speed{}, start_y{}, min_y{}, tumble{};
    Vec3 at{};
};
struct Game {
    std::uint64_t leader{};
    std::uint32_t id{};
    bool leading{};
    Settings settings;
    std::vector<Tag> tags;                // Graffiti: every tag's shape, by index (the leader sends them)
    std::size_t tags_sent{};              // leading: shapes already sent
    std::uint64_t tags_resent{};          // leading: when all were last sent again, for late arrivals
    std::vector<std::uint64_t> joiners; // leading, before the start: who answered the setup
    std::optional<Referee> referee;     // leading, from the start
    std::optional<Message> state;       // the latest state (the leader's own when leading)
    std::uint64_t heard{}, setup_sent{}, state_sent{}, join_sent{};
    std::uint32_t sequence{}, calls_seen{};
    // Leader: the sequence of the positions it gives its referee from the session's poses. Far
    // above any player's own event sequence, so neither is ever taken for an old one.
    std::uint32_t position_sequence = 1u << 30;
    int checkpoint_sent{-1};
    bool lined_up{}; // Deathrace: put at the start gate for this game's countdown
    bool announced{}; // the winner went to chat
    std::uint64_t checkpoint_at{};
    // Deathrace: the checkpoints this player was last told about, and the side of the next gate
    // they were on (to notice them going past it outside the posts).
    int heard_score{}, missed_warned{-1};
    float along{};
    bool have_along{};
    // The next gate was crossed between its posts (at `crossed_at`, on its line): reported until
    // the referee counts it, since a message can be lost.
    bool gate_crossed{};
    Vec3 crossed_at{};
};
// Another player's game, heard from its leader's setup messages: offered like a throwdown drop
// (a banner over its spot, an announcement) and joined only when the player chooses to.
struct Offer {
    std::uint64_t leader{};
    std::uint32_t game{};
    Settings settings;
    std::uint64_t heard{};
    Phase phase = Phase::setup;
    std::size_t players = 1;
    // A leader's state message has told its phase: until then a game heard mid-way (setup
    // messages keep coming while it is played) is not offered as open.
    bool confirmed{};
};
struct State {
    std::optional<Game> game;
    std::vector<Offer> offers;
    // Skate Tag: every player's last position and when it came; when ours last went out; who was it.
    std::map<std::uint64_t, std::pair<Vec3, std::uint64_t>> positions;
    std::uint64_t position_sent{}, last_it{};
    // The latest announcement ("X is starting Hall of Meat"): shown for a while with the join button.
    std::uint64_t invite_at{}, invite_leader{};
    std::uint32_t invite_game{};
    std::uint64_t join_hold_since{}; // A / X held (with something to join) since then
    bool join_key_previous{};
    std::optional<std::pair<std::uint64_t, std::uint32_t>> join_target; // what the join button would join now
    std::set<std::pair<std::uint64_t, std::uint32_t>> sat_out; // games the local player left, or seen end: not offered again
    std::uint64_t local{}, world{};
    bool barred{}, session{};
    std::string local_name;
    std::map<std::uint64_t, std::string> names;
    std::set<std::uint64_t> present; // the other players here now
    // The local skater.
    bool jump_known{}, wipeouts_known{};
    std::uint64_t jump_serial{}, wipeouts{}, last_tick{};
    float recent_speed{}, previous_vertical{};
    float fall_peak{}; // the fastest fall (negative) since the skater last stood still vertically
    std::deque<std::pair<std::uint64_t, float>> heights; // the skater's height over the last 3 s, outside bails
    bool previous_airborne{}, ragdoll_lost{};
    std::uint32_t previous_state{};
    Vec3 position{};
    bool skater{};
    Line line;
    Bail bail;
    std::optional<Vec3> last_inside;
    std::uint64_t outside_since{};
    std::string popup;
    std::uint64_t popup_at{};
    bool version_noticed{};
    // Who in the session has game modes: heard from (a hello or anything else) and when, and the
    // version of those whose messages this game cannot read; each told about once.
    std::map<std::uint64_t, std::uint64_t> modded;
    std::map<std::uint64_t, std::uint8_t> other_version;
    std::set<std::uint64_t> version_told;
    std::uint64_t hello_sent{};
    std::vector<std::string> notices;
    std::vector<std::vector<std::uint8_t>> outbox;
    // Placing the area or the points on the skater, the way skate. sets up a jam session: skate
    // there, size the circle with the D-pad (or PgUp/PgDn), confirm with D-pad Right (or Enter).
    enum class Placing { none, circle, corners, points } placing{};
    float draft_radius = 20.0f;
    float heading{};     // the skater's heading (degrees, the trainer's)
    float yaw_offset{};  // Deathrace: how far the gate being placed is turned from the heading
    float draft_width = 6.0f; // Deathrace: the half width of the gate being placed (metres)
    // Where the next thing goes, like skate.'s quick drop: with `free_place` the free camera flies
    // and the spot is the ground it looks at; without, the ground under the skater.
    bool free_place = true, freecam_ours{}, dpad_release{};
    // Hall of Meat bounce (Skate 3): how much of a hit's speed a ragdoll gets back upward; `mode bounce`.
    float bounce = 0.55f;
    bool bounce_always{}; // on every bail, not only in a Hall of Meat game
    std::uint64_t freecam_off_until{}, freecam_off_sent{};
    Vec3 cursor{};
    float cursor_heading{};
    bool cursor_ok{};
    std::uint32_t pad_previous{};
    std::array<bool, 3> keys_previous{}; // Enter, Backspace, End
    std::uint64_t placing_tick{};
    // `mode debug states on`: every physics state change goes to the log (finding grinds and manuals).
    bool log_states{};
    std::uint64_t results_preview_until{}; // `mode results`: a sample results screen until then
    std::uint32_t logged_state{};
    // S.K.A.T.E.: the local player's attempt this turn, built from skate.'s trick list (its names, their
    // FS / BS sides) until a landing ends it or a bail misses it, and whose turn the camera follows.
    struct SkateTurn {
        std::uint32_t key{};   // the turn (the state's call serial when it began): a new one starts over
        bool mine{}, sent{};
        std::uint64_t since{};
        std::vector<std::pair<std::uint64_t, std::string>> parts; // trick list entry, its name
        std::map<std::uint64_t, std::string> sides;               // entry: "FS" or "BS"
        std::uint64_t spectating{};
    } skate;
    bool spectate = true; // `mode spectate on|off`: watch whoever is up in S.K.A.T.E.
    bool goofy{};         // `mode stance regular|goofy`: S.K.A.T.E.'s flick diagrams mirrored for goofy
    bool was_infected{};  // Infection: the local player was infected at the last tick
    bool was_hiding{};    // Hide & Seek: the hiding time was still running at the last tick
    // The lobby leaderboard: every finished game of two or more players seen in this session (the
    // local player's, or any other's whose state reaches here), each once. 1st 3 points, 2nd 2,
    // 3rd 1. Cleared when the session ends.
    struct LobbyEntry {
        std::string name;
        int points{}, wins{}, podiums{}, games{}, streak{}, best_streak{};
        std::map<Mode, int> mode_wins;
    };
    struct LobbyGame {
        Mode mode{};
        std::string winner, second;
        int players{};
        std::uint64_t at{};
    };
    std::map<std::uint64_t, LobbyEntry> lobby;
    std::set<std::pair<std::uint64_t, std::uint32_t>> lobby_counted;
    std::deque<LobbyGame> lobby_recent; // newest first, at most 6
    std::map<Mode, int> lobby_played;
    std::uint64_t lobby_leader{};
    std::uint64_t diagram_until{}; // `mode diagram <trick>`: its flick diagram on screen until then
    std::string diagram_trick;
    std::mutex hud_mutex;
    overlay::ModesHud hud;
    overlay::ModesMenu menu;
    NativeMatch native; // for skate.'s own HUD widgets (under hud_mutex)
    // A native Throwdown flag placed for the game led here (its key), and the way it faces: the
    // game starts at the flag rather than wherever the leader stands when they press Start.
    std::uint64_t flag_key{};
    float flag_yaw{};
    // For the native menu's thread: the game a flag may be placed for (published each tick), and a
    // placed flag waiting for the game thread (under hud_mutex).
    std::atomic<std::uint64_t> flaggable{};
    std::atomic<bool> counting_down{};
    std::optional<std::pair<Vec3, float>> pending_flag;
};
// `mode grid on|off`: the Throwdowns cards in a grid (seen working in game, modes.13) or one row.
std::atomic<bool> throwdown_grid_on{true};
State &state() {
    static auto *value = new State;
    return *value;
}
std::uint64_t now_ms() { return GetTickCount64(); }
std::uint64_t self_id(const State &s) { return s.local ? s.local : 1; }
std::string name_of(const State &s, std::uint64_t id) {
    if (id == self_id(s)) return s.local_name.empty() ? std::string("You") : s.local_name;
    const auto found = s.names.find(id);
    return found != s.names.end() && !found->second.empty() ? found->second : std::string("Player");
}
std::string grouped(std::int64_t value) {
    auto text = std::to_string(value < 0 ? -value : value);
    for (int i = static_cast<int>(text.size()) - 3; i > 0; i -= 3) text.insert(static_cast<std::size_t>(i), ",");
    return value < 0 ? "-" + text : text;
}
std::string clock_text(std::uint32_t ms) {
    const auto seconds = (ms + 999) / 1000;
    return std::format("{}:{:02}", seconds / 60, seconds % 60);
}
constexpr std::uint32_t colour(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return 0xff000000U | (std::uint32_t{b} << 16) | (std::uint32_t{g} << 8) | r;
}
// Every machine gives a player the same colour: by their place among the game's players' ids.
std::uint32_t player_colour(const Message *state, std::uint64_t id) {
    static constexpr std::uint32_t palette[]{colour(255, 92, 66),  colour(64, 160, 255), colour(255, 206, 64),
                                             colour(90, 220, 120), colour(200, 110, 255), colour(255, 140, 200),
                                             colour(80, 230, 230), colour(255, 160, 60)};
    std::vector<std::uint64_t> ids;
    if (state)
        for (const auto &s : state->standings) ids.push_back(s.player);
    std::sort(ids.begin(), ids.end());
    const auto at = std::find(ids.begin(), ids.end(), id);
    const auto index = at == ids.end() ? static_cast<std::size_t>(id % 8) : static_cast<std::size_t>(at - ids.begin());
    return palette[index % std::size(palette)];
}
void send(State &s, const Message &m) {
    if (!s.session) return;
    try { s.outbox.push_back(encode(m)); } catch (...) {}
}
Message header(const Game &g, Message::Kind kind) {
    Message m;
    m.kind = kind;
    m.leader = g.leader;
    m.game = g.id;
    return m;
}
const Standing *standing(const Game &g, std::uint64_t id) {
    if (!g.state) return nullptr;
    for (const auto &s : g.state->standings)
        if (s.player == id) return &s;
    return nullptr;
}
bool playing(const Game &g) { return g.state && g.state->phase == Phase::playing; }
// Hide & Seek: the local player seeks while the others still hide, so sees nothing and cannot move.
bool seeker_blind(const State &s) {
    if (!s.game || !playing(*s.game) || s.game->settings.mode != Mode::hide || s.game->state->target <= 0) return false;
    const auto *me = standing(*s.game, self_id(s));
    return me && me->up && !me->out;
}
void notice(State &s, std::string text) { s.notices.push_back(std::move(text)); }
void end_game(State &s, std::string why) {
    if (!why.empty()) notice(s, std::move(why));
    s.game.reset();
    s.line = {};
    s.outside_since = 0;
}
// The local player's own line, bail or checkpoint: straight into the referee when leading,
// else to the leader.
void report(State &s, Event event, std::int32_t value, std::int32_t extra, const Vec3 &at, const std::vector<Tag> &tags = {},
            const std::string &trick = {}) {
    if (!s.game || !playing(*s.game) || !standing(*s.game, self_id(s))) return;
    auto &g = *s.game;
    ++g.sequence;
    if (g.referee) {
        g.referee->event(self_id(s), event, value, at, g.sequence, now_ms(), tags, trick);
        return;
    }
    auto m = header(g, Message::Kind::event);
    m.event = event;
    m.value = value;
    m.extra = extra;
    m.at = at;
    m.sequence = g.sequence;
    m.tags = tags;
    m.trick = trick;
    send(s, m);
}
void popup(State &s, std::string text) {
    s.popup = std::move(text);
    s.popup_at = now_ms();
}

// Deathrace cues, soft like Skate 3's: a rising two-note chime through a checkpoint, an arpeggio at
// the finish, two low notes for a missed gate. Built once as small WAVs and played asynchronously.
enum class Cue { checkpoint, finish, missed, invite };
std::vector<std::uint8_t> make_cue(std::initializer_list<std::pair<float, float>> notes, float volume, float overtone) {
    constexpr std::uint32_t rate = 44100;
    std::vector<std::int16_t> samples;
    for (const auto &[frequency, seconds] : notes) {
        const auto count = static_cast<std::size_t>(seconds * rate) + rate / 8; // each note rings into the next
        const auto start = samples.size() >= rate / 8 ? samples.size() - rate / 8 : samples.size();
        samples.resize(std::max(samples.size(), start + count));
        for (std::size_t i = 0; i < count; ++i) {
            const float t = static_cast<float>(i) / rate;
            const float envelope = std::min(1.0f, t / 0.006f) * std::exp(-t * 9.0f);
            const float wave = std::sin(6.2831853f * frequency * t) + overtone * std::sin(6.2831853f * frequency * 2 * t);
            const float mixed = samples[start + i] / 32767.0f + wave * envelope * volume;
            samples[start + i] = static_cast<std::int16_t>(std::clamp(mixed, -1.0f, 1.0f) * 32767);
        }
    }
    const auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
    std::vector<std::uint8_t> wav(44 + bytes);
    const auto put = [&](std::size_t at, std::uint32_t value, int size) {
        for (int i = 0; i < size; ++i) wav[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    std::memcpy(wav.data(), "RIFF", 4);
    put(4, 36 + bytes, 4);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put(16, 16, 4);
    put(20, 1, 2);        // PCM
    put(22, 1, 2);        // mono
    put(24, rate, 4);
    put(28, rate * 2, 4); // bytes per second
    put(32, 2, 2);
    put(34, 16, 2);
    std::memcpy(wav.data() + 36, "data", 4);
    put(40, bytes, 4);
    std::memcpy(wav.data() + 44, samples.data(), bytes);
    return wav;
}
void play(Cue cue) {
    static const auto checkpoint = make_cue({{1318.5f, 0.07f}, {1760.0f, 0.18f}}, 0.16f, 0.15f);
    static const auto finish = make_cue({{1046.5f, 0.08f}, {1318.5f, 0.08f}, {1568.0f, 0.08f}, {2093.0f, 0.3f}}, 0.16f, 0.15f);
    static const auto missed = make_cue({{392.0f, 0.12f}, {293.7f, 0.25f}}, 0.2f, 0.35f);
    static const auto invite = make_cue({{880.0f, 0.09f}, {1174.7f, 0.09f}, {1318.5f, 0.22f}}, 0.12f, 0.1f);
    const auto &wav = cue == Cue::checkpoint ? checkpoint : cue == Cue::finish ? finish : cue == Cue::invite ? invite : missed;
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
// The leader's state before the start: who is in, nothing scored yet.
Message setup_state(const State &s, const Game &g) {
    auto m = header(g, Message::Kind::state);
    m.phase = Phase::setup;
    m.standings.push_back({self_id(s)});
    for (const auto id : g.joiners) m.standings.push_back({id});
    return m;
}
std::uint32_t default_duration(Mode mode) {
    switch (mode) {
    case Mode::meat: return 180;
    case Mode::race: return 300;
    case Mode::tag: return 240;
    case Mode::infection: return 300;
    case Mode::hide: return 360;
    default: return 300;
    }
}

// ---- the trick logger ----------------------------------------------------------------
// `mode tricklog on|off`, for finding where skate. names the tricks it scores: every write to its UI
// models while on (the score HUD's trick list among them) that carries text, inline, behind a
// pointer or two, ASCII or UTF-16, goes to the log, with the field's handle and its type. Fields
// written rarely (as a trick's name or count would be, not a clock) also log their bytes when they
// change. Off, it reports how many writes each type had.
struct TrickLog {
    std::atomic<bool> on{};
    std::mutex mutex;
    std::vector<std::string> lines; // waiting for the client tick
    std::map<std::uint64_t, std::pair<unsigned, std::uint64_t>> fields; // handle: writes, last bytes
    std::map<std::string, std::uint64_t> texts;                          // handle+text seen: no repeats
    std::map<std::uintptr_t, std::uint64_t> types;
    std::size_t logged{};
};
TrickLog &trick_log() {
    static auto *value = new TrickLog;
    return *value;
}
// Readable text at `address`: ASCII, or UTF-16 with every other byte zero; empty when not.
std::string text_at(std::uintptr_t address) noexcept {
    std::array<unsigned char, 96> raw{};
    if (!memory::read_bytes(address, raw.data(), raw.size())) return {};
    const auto printable = [](unsigned char c) { return c >= 0x20 && c < 0x7f; };
    std::string text;
    for (std::size_t i = 0; i < raw.size() && raw[i]; ++i) {
        if (!printable(raw[i])) return {};
        text += static_cast<char>(raw[i]);
    }
    if (text.size() >= 3) return text;
    text.clear();
    for (std::size_t i = 0; i + 1 < raw.size() && (raw[i] || raw[i + 1]); i += 2) {
        if (raw[i + 1] || !printable(raw[i])) return {};
        text += static_cast<char>(raw[i]);
    }
    return text.size() >= 3 ? text : std::string{};
}
// ---- the trick feed (S.K.A.T.E.) ----------------------------------------------------------
// skate.'s score HUD lists each trick as it is done, one entry of ScoringHUDViewModel's trick list
// per trick, written through the UI model's write of one value (found with `mode tricklog` on
// 2026-10-09, game build 20260929): a field's handle is its entry (upper bits) and member (low 16
// bits). Member 0x0002 is the trick's name, a string ("Kickflip", "50-50 Grind", "Seatbelt");
// 0x009c its side ("ID_TRICK_FS" / "ID_TRICK_BS", grinds and slides); 0x0034 how it was landed
// ("ID_TRICK_LANDING_CLEAN"), written when the line lands. A grab or grind done in the same air as
// a flip is its own entry before the one landing.
constexpr std::uint32_t trick_name_member = 0x0002, trick_side_member = 0x009c, trick_landing_member = 0x0034;
constexpr std::uintptr_t ui_string_type_rva = 0x765dcc0; // Skate.exe: the UI model's string type
struct TrickNote {
    std::uint64_t entry{};
    char kind{}; // 'n' name, 's' side, 'l' landing
    std::string text;
};
struct TrickFeed {
    std::atomic<bool> on{};
    std::mutex mutex;
    std::vector<TrickNote> notes;
};
TrickFeed &trick_feed() {
    static auto *value = new TrickFeed;
    return *value;
}
void feed_tap(std::uint64_t handle, std::uintptr_t type, const void *value) noexcept {
    auto &f = trick_feed();
    if (!f.on.load(std::memory_order_acquire)) return;
    const auto member = static_cast<std::uint32_t>(handle & 0xffff);
    if (member != trick_name_member && member != trick_side_member && member != trick_landing_member) return;
    static const auto string_type = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + ui_string_type_rva;
    if (member == trick_name_member && type != string_type) return;
    try {
        std::uint64_t first{};
        if (!memory::peek_bytes(reinterpret_cast<std::uintptr_t>(value), &first, sizeof first)) return;
        auto text = text_at(static_cast<std::uintptr_t>(first));
        // A name skate. gives as a short code rather than text (one grind wrote 0x42532030): the code
        // itself, the same on every player's game, so it can still be set and copied. A pointer whose
        // text could not be read is not one: it differs from game to game.
        if (text.empty() && member == trick_name_member && first && first < 0x100000000ull) text = std::format("Trick #{:x}", first);
        if (text.empty()) return;
        char kind = 'n';
        if (member == trick_side_member) {
            if (!text.starts_with("ID_TRICK_")) return;
            kind = 's';
        } else if (member == trick_landing_member) {
            if (!text.starts_with("ID_TRICK_LANDING")) return;
            kind = 'l';
        }
        std::lock_guard lock(f.mutex);
        if (f.notes.size() < 256) f.notes.push_back({handle >> 16, kind, std::move(text)});
    } catch (...) {}
}
std::vector<TrickNote> take_trick_notes() {
    auto &f = trick_feed();
    std::lock_guard lock(f.mutex);
    return std::exchange(f.notes, {});
}

void trick_tap(std::uint64_t handle, std::uintptr_t type, const void *value) noexcept {
    feed_tap(handle, type, value);
    auto &t = trick_log();
    if (!t.on.load(std::memory_order_acquire)) return;
    try {
        std::array<unsigned char, 16> raw{};
        if (!memory::peek_bytes(reinterpret_cast<std::uintptr_t>(value), raw.data(), 8)) return;
        (void)memory::read_bytes(reinterpret_cast<std::uintptr_t>(value) + 8, raw.data() + 8, 8);
        std::uint64_t first{}, second{};
        std::memcpy(&first, raw.data(), 8);
        std::memcpy(&second, raw.data() + 8, 8);
        // The text: in the value itself, behind its first pointer, or behind the pointer that points to.
        std::string text, where;
        if (const auto inline_text = text_at(reinterpret_cast<std::uintptr_t>(value)); !inline_text.empty()) text = inline_text, where = "inline";
        else if (const auto once = text_at(static_cast<std::uintptr_t>(first)); !once.empty()) text = once, where = "*value";
        else {
            std::uint64_t inner{};
            if (memory::read(static_cast<std::uintptr_t>(first), inner))
                if (const auto twice = text_at(static_cast<std::uintptr_t>(inner)); !twice.empty()) text = twice, where = "**value";
        }
        std::lock_guard lock(t.mutex);
        ++t.types[type];
        auto &field = t.fields[handle];
        const bool changed = field.first == 0 || field.second != first;
        ++field.first;
        field.second = first;
        if (t.logged >= 6000) return;
        if (!text.empty()) {
            if (!t.texts.emplace(std::format("{:x}|{}", handle, text), 1).second) return;
            t.lines.push_back(std::format("Trick log: field {:x} (type {:x}) text {} \"{}\"", handle, type, where, text));
            ++t.logged;
        } else if (changed && field.first <= 25) {
            t.lines.push_back(std::format("Trick log: field {:x} (type {:x}) #{} bytes {:016x} {:016x}", handle, type, field.first, first, second));
            ++t.logged;
        }
    } catch (...) {}
}
void flush_trick_log() {
    auto &t = trick_log();
    std::vector<std::string> lines;
    {
        std::lock_guard lock(t.mutex);
        lines.swap(t.lines);
    }
    for (const auto &line : lines) logging::log(logging::Level::info, logging::Channel::runtime, "{}", line);
}
std::string trick_log_command(const std::vector<std::string> &arguments) {
    auto &t = trick_log();
    const bool on = !arguments.empty() && arguments[0] == "on";
    if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off")) return "usage: mode tricklog on|off";
    if (on) {
        {
            std::lock_guard lock(t.mutex);
            t.fields.clear();
            t.texts.clear();
            t.types.clear();
            t.lines.clear();
            t.logged = 0;
        }
        t.on.store(true, std::memory_order_release); // the tick puts the tap on
        logging::log(logging::Level::info, logging::Channel::runtime, "Trick log: on.");
        return "Trick log on. Do a kickflip, a heelflip, a grab, a grind and a manual (a few seconds apart), then `mode tricklog off`.";
    }
    t.on.store(false, std::memory_order_release);
    flush_trick_log();
    std::vector<std::pair<std::uint64_t, std::uintptr_t>> counts;
    {
        std::lock_guard lock(t.mutex);
        for (const auto &[type, count] : t.types) counts.push_back({count, type});
        logging::log(logging::Level::info, logging::Channel::runtime, "Trick log: off; {} fields, {} lines.", t.fields.size(), t.logged);
    }
    std::sort(counts.rbegin(), counts.rend());
    for (std::size_t i = 0; i < counts.size() && i < 30; ++i)
        logging::log(logging::Level::info, logging::Channel::runtime, "Trick log: type {:x} written {} times.", counts[i].second, counts[i].first);
    return "Trick log off. Close the game and tell Claude.";
}

// ---- the local skater ----------------------------------------------------------------
// A Hall of Meat game played with ReSkate's own Hall of Meat (Extension/HallOfMeat), as it made it:
// its skeleton, card and slow motion show each bail and its Meat scores it. Without it (a game
// build it does not know), the Bone Cam and the bail measured here stand in.
bool official_meat() noexcept { return local_meat_game() && hall_of_meat::available(); }
// Bails are watched before anything else: during a ragdoll the trainer may have no skater to
// measure, but the physics state the bail hook reports keeps coming.
void track_bail(State &s, std::uint32_t physics, float vertical, std::uint64_t now, float dt) {
    const auto watch = watched_physics_state();
    // A wipeout loses the line and starts measuring the bail: the counter the trainer keeps for
    // state 300, any other wipeout state (300-399), or a hard fall straight off the board.
    bool bailed = false;
    if (watch.valid) {
        if (!s.wipeouts_known) {
            s.wipeouts = watch.wipeouts;
            s.wipeouts_known = true;
        } else if (watch.wipeouts != s.wipeouts) {
            s.wipeouts = watch.wipeouts;
            bailed = true;
        }
    }
    const auto in = [](std::uint32_t state, std::uint32_t first) { return state >= first && state < first + 100; };
    if (in(physics, wipeout_state) && !in(s.previous_state, wipeout_state)) bailed = true;
    if (physics >= offboard_states_first && in(s.previous_state, 200) && s.previous_vertical < -6.0f) bailed = true;
    if (std::exchange(s.ragdoll_lost, false)) bailed = true;
    // On foot the skater is off the board already, so no state change marks a jump off a roof:
    // a fall faster than 7.5 m/s (a drop of about 3 m) brought to a stop is the slam.
    // A reset (state 700) or a teleport moves the skater, it does not slam them.
    const bool reset = physics == 700 || s.previous_state == 700;
    if (reset) s.fall_peak = 0;
    s.fall_peak = std::min(s.fall_peak, vertical);
    // Off the board: 500-599. Not 600-699, the plants and other on-foot tricks (a handplant is not
    // a fall).
    const bool on_foot = !reset && (in(physics, offboard_states_first) || in(s.previous_state, offboard_states_first));
    // The body stopping dead off the board (a pole, a wall, the ground); on the board the game's
    // own wipeout states say when a landing went wrong. Only when the game itself had the skater
    // going fast (7 m/s and more): the pose's own speed jumps about on stairs, in a jump on the
    // spot and through tricks, and alone it is no proof of anything.
    // Striking an object at speed (a pole, a tree, a rail) off the board, when the game itself had
    // the skater going (6 m/s and more). On the board the game wipes the skater out itself when
    // they really hit something (state 300 and up, a bail already); a landing off a kicker jolts
    // the pose just like a hit, and the game rolls on (state 100 or 200), so that is no bail.
    // Read every tick all the same, so a stale strike is not kept for later.
    if (bone_cam_struck() && on_foot && s.recent_speed >= 6.0f) {
        logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: hit an object at {:.1f} m/s.", s.recent_speed);
        bailed = true;
    }
    const bool slam = bone_cam_slam();
    if (slam && on_foot && s.recent_speed >= 7.0f) {
        logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: on-foot slam (the body stopped dead).");
        bailed = true;
    }
    if (vertical > -1.5f) {
        if (on_foot && s.fall_peak < -7.5f) {
            logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: on-foot fall stopped at {:.1f} m/s.", -s.fall_peak);
            bailed = true;
        }
        s.fall_peak = 0;
    }
    if (bailed && !s.bail.active) {
        logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: bail (state {} -> {}) at {:.1f} m/s.",
                     s.previous_state, physics, s.recent_speed);
        // ReSkate's own Hall of Meat replaces the Bone Cam wherever it runs (its switch, or a Hall of
        // Meat game holding it on); the Bone Cam stands in only on a game build it does not know.
        if (!hall_of_meat::available()) bone_cam_bail();
        if (s.line.tricks) popup(s, "Line lost!");
        s.line = {};
        // The drop counts from the highest the skater was in the last few seconds: a fall is often
        // only noticed once it has landed.
        float top = s.position[1];
        for (const auto &[at, y] : s.heights) top = std::max(top, y);
        s.bail = {true, now, s.recent_speed, top, s.position[1], 0.0f, s.position};
    }
    if (!s.bail.active && s.skater) {
        s.heights.push_back({now, s.position[1]});
        while (!s.heights.empty() && now - s.heights.front().first > 3000) s.heights.pop_front();
    }
    if (reset) s.heights.clear();
    s.previous_state = physics;
    s.previous_vertical = vertical;
    if (s.bail.active) {
        s.bail.min_y = std::min(s.bail.min_y, s.position[1]);
        if (in(physics, wipeout_state) || physics >= offboard_states_first) s.bail.tumble += dt;
        if (now - s.bail.started >= bail_settle_ms) {
            const auto score = bail_score({s.bail.speed, s.bail.start_y - s.bail.min_y, s.bail.tumble});
            s.bail.active = false;
            // Where the body came to rest: a dive off a building outside the area still lands in it.
            s.bail.at = s.position;
            const auto bounces = take_bail_bounces();
            logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: bail scored {} ({:.1f} m/s, {:.1f} m drop, {:.1f} s, {} bounce{}{}).",
                         score, s.bail.speed, s.bail.start_y - s.bail.min_y, s.bail.tumble, bounces.count, bounces.count == 1 ? "" : "s",
                         bounces.count ? std::format(", hardest hit {:.1f} m/s", bounces.hardest) : std::string());
            // What the bounce saw since the last bail, to tell why it did or did not bounce.
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Game modes: bounce check: {} physics steps with it on, {} in a state it acts in, {} reading the body, fastest fall {:.1f} m/s{}{}.",
                         bounces.steps, bounces.eligible, bounces.read, bounces.fastest, bounces.why.empty() ? "" : ", last problem: ", bounces.why);
            // Hall of Meat's own Meat scores the bail instead (in the tick, below).
            if (!official_meat()) {
                report(s, Event::bail, score, 0, s.bail.at);
                if (s.game && s.game->settings.mode == Mode::meat) popup(s, "Meat: " + grouped(score));
            }
        }
    }
}

void track_skater(State &s, bool in_world, std::uint64_t now) {
    const auto t = trainer::telemetry();
    const float dt = s.last_tick && now > s.last_tick ? std::min(0.25f, (now - s.last_tick) / 1000.0f) : 0.0f;
    s.last_tick = now;
    const bool skater = in_world && t.skater;
    if (skater != s.skater && (s.bail.active || s.log_states))
        logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: the skater is {}.", skater ? "back" : "unavailable");
    // A full ragdoll takes the skater away from the trainer mid-slam: losing it in the air or at speed is a bail.
    if (!skater && s.skater && in_world && !s.bail.active && (s.previous_airborne || s.recent_speed > 2.0f)) s.ragdoll_lost = true;
    s.skater = skater;
    if (skater) {
        s.position = t.position;
        s.heading = t.heading;
        s.previous_airborne = t.airborne;
    }
    const auto physics_watch = watched_physics_state();
    const auto physics = physics_watch.valid ? physics_watch.state : t.physics_state;
    // `mode debug states on`: every physics state change, on the board or off it.
    if (s.log_states && in_world && physics != s.logged_state) {
        logging::log(logging::Level::info, logging::Channel::runtime,
                     "Game modes state: {} -> {} at {:.2f}, {:.2f}, {:.2f}, {:.1f} m/s, up {:.1f}{}{}", s.logged_state, physics,
                     t.position[0], t.position[1], t.position[2], t.speed, t.vertical, t.airborne ? " (airborne)" : "",
                     skater ? "" : " (no skater)");
        s.logged_state = physics;
    }    if (in_world) track_bail(s, physics, skater ? t.vertical : s.previous_vertical, now, dt);
    if (!s.skater) {
        s.line = {};
        s.jump_known = false;
        return;
    }
    s.recent_speed = std::max(t.speed, s.recent_speed - 15.0f * dt);

    // A jump the Trainer finished measuring since the last tick is a trick in the line.
    if (!s.jump_known) {
        s.jump_serial = t.last.serial;
        s.jump_known = true;
    } else if (t.last.serial != s.jump_serial) {
        s.jump_serial = t.last.serial;
        const auto score = trick_score({t.last.air_time, t.last.height, t.last.distance, t.last.spin, t.last.flip,
                                        t.last.board_turn});
        if (score > 0 && !s.bail.active) {
            ++s.line.tricks;
            s.line.sum += score;
            s.line.landed = now;
            s.line.at = t.last.landing;
            // Down a stair set or a gap: landed a metre or more below the takeoff.
            const bool gap = t.last.takeoff[1] - t.last.landing[1] >= gap_drop;
            if (gap && s.line.tags.size() < max_line_tags) s.line.tags.push_back({TagKind::gap, {t.last.takeoff, t.last.landing}});
            popup(s, "+" + grouped(score) + (gap ? "  gap" : ""));
        }
    }
    // Grinds and slides: the path the board slides along is a trick in the line, and in Graffiti
    // a tag on whatever was ground.
    const bool grinding = t.physics_state >= grind_states_first && t.physics_state < grind_states_end;
    if (grinding && !s.bail.active) {
        if (s.line.grind.empty()) s.line.grind_started = now;
        const auto &last = s.line.grind.empty() ? t.position : s.line.grind.back();
        const float dx = t.position[0] - last[0], dy = t.position[1] - last[1], dz = t.position[2] - last[2];
        if (s.line.grind.empty() || dx * dx + dy * dy + dz * dz >= 0.35f * 0.35f) s.line.grind.push_back(t.position);
        s.line.landed = now; // the line lives on while grinding
    } else if (!s.line.grind.empty()) {
        auto &path = s.line.grind;
        if (path.size() == 1) path.push_back(t.position);
        float metres = 0;
        for (std::size_t i = 1; i < path.size(); ++i) {
            const float dx = path[i][0] - path[i - 1][0], dz = path[i][2] - path[i - 1][2];
            metres += std::sqrt(dx * dx + dz * dz);
        }
        const float seconds = (now - s.line.grind_started) / 1000.0f;
        if (!s.bail.active && (seconds >= 0.2f || metres >= 0.6f)) {
            const auto score = grind_score(seconds, metres);
            ++s.line.tricks;
            s.line.sum += score;
            s.line.landed = now;
            s.line.at = path.back();
            if (s.line.tags.size() < max_line_tags) s.line.tags.push_back({TagKind::grind, thin_path(path)});
            popup(s, "+" + grouped(score) + "  grind");
        }
        path.clear();
    }
    // A line ends once the skater has been on the ground a moment, or steps off the board.
    if (s.line.tricks && !t.airborne && !s.bail.active && s.line.grind.empty() &&
        (now - s.line.landed >= line_grace_ms || t.physics_state >= offboard_states_first)) {
        const auto score = line_score(s.line.sum, s.line.tricks);
        report(s, Event::line, score, static_cast<std::int32_t>(s.line.tricks), s.line.at, s.line.tags);
        popup(s, std::format("{} line: {}", s.line.tricks == 1 ? std::string("1 trick") : std::format("{} trick", s.line.tricks),
                             grouped(score)));
        s.line = {};
    }
}

// A game's identity across machines (its leader and id).
std::uint64_t game_key(const Game &g) { return g.leader * 0x9E3779B97F4A7C15ULL ^ g.id; }

// The countdown: everyone in the game is put at the start, spread out so nobody lands on anyone,
// in the same order on every machine (where the session allows teleporting; elsewhere they skate
// there). Deathrace lines up across its start gate; a circle's players stand in a ring around its
// centre; anything else rings the spot the leader started the game from.
// Teleports the skater a little above `at`, facing `heading` (the trainer's degrees: facing
// sin, cos on x and z).
// False when the teleport was refused (a bail, a menu, a session that forbids it).
bool teleport_to(const Vec3 &at, float heading) {
    return !trainer::command("tp", {std::format("{:.2f}", at[0]), std::format("{:.2f}", at[1] + 0.3f), std::format("{:.2f}", at[2]),
                                    std::format("{:.1f}", heading)})
                .starts_with("error");
}
float heading_towards(const Vec3 &from, const Vec3 &to) {
    return std::atan2(to[0] - from[0], to[2] - from[2]) * 180.0f / 3.14159265f;
}
// Which way Deathrace gate i faces: its own facing, else along the route (the start towards the
// next gate, any other from the gate before it).
float gate_heading(const Settings &st, std::size_t i) {
    if (i < st.yaws.size()) return st.yaws[i];
    if (st.points.size() < 2 || i >= st.points.size()) return 0.0f;
    return i > 0 ? heading_towards(st.points[i - 1], st.points[i]) : heading_towards(st.points[0], st.points[1]);
}
void line_up(State &s) {
    if (!s.game || !s.skater) return;
    auto &g = *s.game;
    if (g.lined_up || !g.state || g.state->phase != Phase::countdown || !standing(g, self_id(s))) return;
    g.lined_up = true;
    std::vector<std::uint64_t> ids;
    for (const auto &p : g.state->standings) ids.push_back(p.player);
    std::sort(ids.begin(), ids.end());
    const auto count = static_cast<float>(ids.size());
    const auto index = static_cast<float>(std::find(ids.begin(), ids.end(), self_id(s)) - ids.begin());
    Vec3 spot{};
    if (g.settings.mode == Mode::race && g.settings.points.size() >= 2) {
        const auto &start = g.settings.points[0];
        const float yaw = gate_heading(g.settings, 0) * 3.14159265f / 180.0f, dx = std::sin(yaw), dz = std::cos(yaw);
        // Side by side across the gate, a few steps behind its line, facing through it.
        const float across = (index - (count - 1.0f) * 0.5f) * 2.5f;
        spot = {start[0] - dz * across - dx * 3.0f, start[1], start[2] + dx * across - dz * 3.0f};
    } else {
        Vec3 centre{};
        if (g.settings.area_radius > 0 && !g.settings.corners.empty()) centre = g.settings.corners[0];
        else if (g.settings.has_spawn) centre = g.settings.spawn;
        else if (has_area(g.settings)) centre = area_centre(g.settings.corners);
        else return;
        // Skate Tag spreads everyone well out, so whoever starts it has a chase on their hands.
        const float spread = tag_like(g.settings.mode) ? 15.0f + 2.0f * count : 2.0f + 0.6f * count;
        const float ring = count <= 1 ? 0.0f : spread, angle = 6.2831853f * index / std::max(1.0f, count);
        spot = {centre[0] + std::cos(angle) * ring, centre[1], centre[2] + std::sin(angle) * ring};
        // Round the circle everyone faces its middle; alone, the way the leader set it up.
        const bool flagged = s.flag_key == game_key(g);
        teleport_to(spot, ring > 0 ? heading_towards(spot, centre) : flagged ? s.flag_yaw : s.heading);
        return;
    }
    teleport_to(spot, gate_heading(g.settings, 0)); // down the start gate
}
// Where a game's banner stands and its joiners are sent: Deathrace's start gate, the circle's or
// area's centre, its first spot, or where its leader set it up.
std::optional<Vec3> game_spot(const Settings &st) {
    if (st.mode == Mode::race && !st.points.empty()) return st.points.front();
    if (st.area_radius > 0 && !st.corners.empty()) return st.corners.front();
    if (has_area(st)) return area_centre(st.corners);
    if (!st.points.empty()) return st.points.front();
    if (st.has_spawn) return st.spawn;
    return std::nullopt;
}
bool offer_open(const Offer &o) { return o.confirmed && (o.phase == Phase::setup || o.phase == Phase::countdown); }
// Joins an offered game: the leader is asked at once, and the player is put at its start, a few
// steps from its spot (round the circle, or behind Deathrace's start gate), not on anyone's head.
std::string join_offer(State &s, const Offer &offer, std::uint64_t now) {
    if (s.barred) return "error: your mods change trick scoring, so you cannot play in this session.";
    if (!offer_open(offer)) return "error: that game is already under way.";
    if (s.game && s.game->leading) return "error: you lead a game: `mode stop` it first.";
    // A game joined but not heard from yet counts too: replacing it silently would leave its leader
    // counting this player in.
    if (s.game && (!s.game->state || s.game->state->phase != Phase::results))
        return "error: you are in " + name_of(s, s.game->leader) + "'s game: `mode leave` first.";
    Game g;
    g.leader = offer.leader;
    g.id = offer.game;
    g.settings = offer.settings;
    g.heard = now;
    g.join_sent = now;
    s.game = std::move(g);
    send(s, header(*s.game, Message::Kind::join));
    const auto text = std::format("Joined {}'s {}!", name_of(s, offer.leader), mode_name(offer.settings.mode));
    if (const auto spot = game_spot(offer.settings); spot && s.skater) {
        const float angle = static_cast<float>(self_id(s) % 12) * 0.5236f; // twelve places round it
        Vec3 at{(*spot)[0] + std::cos(angle) * 3.0f, (*spot)[1], (*spot)[2] + std::sin(angle) * 3.0f};
        float facing = heading_towards(at, *spot);
        if (offer.settings.mode == Mode::race) {
            facing = gate_heading(offer.settings, 0);
            const float yaw = facing * 3.14159265f / 180.0f, side = (static_cast<float>(self_id(s) % 5) - 2.0f) * 2.0f;
            at = {(*spot)[0] - std::sin(yaw) * 3.0f - std::cos(yaw) * side, (*spot)[1], (*spot)[2] - std::cos(yaw) * 3.0f + std::sin(yaw) * side};
        }
        teleport_to(at, facing);
    }
    std::erase_if(s.offers, [&](const Offer &o) { return o.leader == offer.leader; });
    s.invite_at = 0;
    popup(s, text);
    return text;
}
// Offers drop when their leader is gone or quiet, or their game ends. While not in a game, the
// join button (A / X held on a pad, J tapped on the keyboard) joins the one just announced (for
// 15 s), or else one whose banner the skater stands within 30 m of. A / X is skate.'s push, so a
// tap never joins: it must be held.
bool game_window_focused();
constexpr std::uint64_t invite_ms = 15000, join_hold_ms = 800;
constexpr std::uint32_t pad_a = 0x1000; // XINPUT_GAMEPAD_A: A on an Xbox pad, X (cross) on a PlayStation one
void run_invites(State &s, std::uint64_t now) {
    // A game seen reach its results is not offered again by the setup messages its leader keeps
    // sending until it closes.
    for (const auto &o : s.offers)
        if (o.phase == Phase::results) s.sat_out.insert({o.leader, o.game});
    std::erase_if(s.offers, [&](const Offer &o) {
        return now - o.heard > leader_timeout_ms || (s.session && !s.present.contains(o.leader)) || o.phase == Phase::results ||
               s.sat_out.contains({o.leader, o.game}) || (s.game && s.game->leader == o.leader && s.game->id == o.game);
    });
    s.join_target.reset();
    const bool free = !s.game || (s.game->state && s.game->state->phase == Phase::results);
    if (free && s.placing == State::Placing::none) {
        const Offer *pick{};
        if (now - s.invite_at < invite_ms)
            for (const auto &o : s.offers)
                if (o.leader == s.invite_leader && o.game == s.invite_game && offer_open(o)) pick = &o;
        if (!pick && s.skater) {
            float best = 30.0f;
            for (const auto &o : s.offers)
                if (const auto spot = game_spot(o.settings); spot && offer_open(o)) {
                    const float d = std::hypot((*spot)[0] - s.position[0], (*spot)[2] - s.position[2]);
                    if (d < best && std::abs((*spot)[1] - s.position[1]) < 15.0f) best = d, pick = &o;
                }
        }
        if (pick) s.join_target = std::make_pair(pick->leader, pick->game);
    }
    ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad);
    const bool a_held = pad.available && (pad.buttons & pad_a);
    // The hold starts once there is something to join: pushing up to a banner does not count.
    if (!a_held || !s.join_target) s.join_hold_since = 0;
    else if (!s.join_hold_since) s.join_hold_since = now;
    const bool a = s.join_hold_since && now - s.join_hold_since >= join_hold_ms;
    const bool key = overlay::key_down('J');
    const bool j = key && !s.join_key_previous;
    s.join_key_previous = key;
    if (!s.join_target || !(a || j) || overlay::interface_open() || !game_window_focused()) return;
    if (a) s.join_hold_since = now; // one try per hold
    for (const auto &o : s.offers)
        if (o.leader == s.join_target->first && o.game == s.join_target->second) {
            const auto offer = o; // join_offer drops it from the list
            (void)join_offer(s, offer, now);
            return;
        }
}
std::string gate_name(std::size_t index, std::size_t count);
// Deathrace: the next checkpoint reached. The area: a player outside it for a while is put back.
void track_game(State &s, std::uint64_t now) {
    if (!s.game || !s.skater || !playing(*s.game)) {
        s.outside_since = 0;
        return;
    }
    auto &g = *s.game;
    const auto *me = standing(g, self_id(s));
    if (!me || me->out) return;
    if (tag_like(g.settings.mode)) {
        // Where everyone is comes from the session's own skater poses (tick() keeps s.positions),
        // not from messages of ours: the leader's referee reads them about 7 times a second.
        s.positions[self_id(s)] = {s.position, now};
        if (g.referee && g.state && now - s.position_sent >= 150) {
            s.position_sent = now;
            for (const auto &p : g.state->standings) {
                const auto found = s.positions.find(p.player);
                if (found == s.positions.end() || now - found->second.second > position_fresh_ms) continue;
                g.referee->event(p.player, Event::position, 0, found->second.first, ++g.position_sequence, now);
            }
        }
    }
    // Infection: the moment the local player is caught.
    if (g.settings.mode == Mode::infection) {
        if (me->up && !s.was_infected) {
            play(Cue::missed);
            popup(s, "YOU'RE INFECTED! Hunt the survivors");
        }
        s.was_infected = me->up;
    }
    // Hide & Seek: found, and the seekers let loose.
    if (g.settings.mode == Mode::hide && g.state) {
        if (me->up && !s.was_infected && g.state->target <= 0) {
            play(Cue::missed);
            popup(s, "FOUND! Now you seek too");
        }
        s.was_infected = me->up;
        const bool hiding = g.state->target > 0;
        if (s.was_hiding && !hiding) {
            play(Cue::invite);
            popup(s, me->up ? "GO FIND THEM!" : "THEY'RE COMING!");
        }
        s.was_hiding = hiding;
    }
    if (g.settings.mode == Mode::tag) {
        const auto it = g.state ? g.state->turn : 0;
        if (it && it != s.last_it) {
            if (it == self_id(s)) {
                play(Cue::missed);
                popup(s, "YOU'RE IT! Tag someone");
            } else if (s.last_it == self_id(s)) {
                play(Cue::checkpoint);
                popup(s, "Tagged " + name_of(s, it) + "! Run!");
            }
            s.last_it = it;
        }
    }
    if (g.settings.mode == Mode::race) {
        const auto count = g.settings.points.size();
        // The referee counted a gate: the chime (the finish has its own).
        if (me->score > g.heard_score) {
            g.heard_score = me->score;
            g.have_along = false;
            g.gate_crossed = false;
            if (me->score > 0) {
                const bool finished = static_cast<std::size_t>(me->score) >= count;
                play(finished ? Cue::finish : Cue::checkpoint);
                // Gate 0 is the start; checkpoints are 1 .. count-2; the finish is last.
                if (!finished && me->score >= 2)
                    popup(s, me->score + 1 == static_cast<int>(count) ? std::string("Checkpoint! Now the finish")
                                                                      : std::format("Checkpoint {}/{}", me->score - 1, count - 2));
            }
        }
    }
    if (g.settings.mode == Mode::race && me->score >= 0 && static_cast<std::size_t>(me->score) < g.settings.points.size()) {
        const auto index = static_cast<std::size_t>(me->score);
        const auto &next = g.settings.points[index];
        const float dx = s.position[0] - next[0], dz = s.position[2] - next[2];
        const float half = gate_half_width(g.settings, index);
        // A gate counts when the skater crosses its line, forwards, between its posts: not on
        // getting near it. Its facing is its own (or along the route).
        const float yaw = gate_heading(g.settings, index) * 3.14159265f / 180.0f;
        const float fx = std::sin(yaw), fz = std::cos(yaw);
        const float along = dx * fx + dz * fz, across = std::abs(dx * fz - dz * fx);
        const bool level = std::abs(s.position[1] - next[1]) <= 10.0f;
        // Crossing the line either way counts (as in Skate 3: a gate placed facing back the way the
        // route comes is still a gate). The start also counts for a skater already just past its
        // line when the race begins (rolled over it during the countdown).
        const bool crossed = g.have_along ? (g.along < 0) != (along < 0) && std::abs(along) < 6.0f && std::abs(g.along) < 6.0f
                                          : index == 0 && along >= 0 && along < 15.0f;
        if (crossed && across <= half && level && !g.gate_crossed) {
            g.gate_crossed = true;
            g.crossed_at = {s.position[0] - fx * along, s.position[1], s.position[2] - fz * along}; // on the line
        }
        // Reported (again every 1.5 s) until the referee counts it: a lost message must not cost the gate.
        if (g.gate_crossed && (g.checkpoint_sent != me->score || now - g.checkpoint_at > 1500)) {
            g.checkpoint_sent = me->score;
            g.checkpoint_at = now;
            report(s, Event::checkpoint, me->score, 0, g.crossed_at);
        }
        // Missed: across the gate's line but outside its posts, or already through a later gate.
        // (Not while a crossing waits for the referee: the skater rides on to the next gate meanwhile.)
        bool missed = index > 0 && crossed && across > half && across < half + 20.0f && level;
        g.along = along;
        g.have_along = true;
        for (std::size_t later = index + 1; later < g.settings.points.size() && !missed && !g.gate_crossed; ++later) {
            const auto &p = g.settings.points[later];
            if (std::hypot(s.position[0] - p[0], s.position[2] - p[2]) <= gate_half_width(g.settings, later) * 0.5f &&
                std::abs(s.position[1] - p[1]) <= 10.0f)
                missed = true;
        }
        if (missed && g.missed_warned != me->score) {
            g.missed_warned = me->score;
            play(Cue::missed);
            popup(s, std::format("Missed {}! Go back", gate_name(index, g.settings.points.size())));
        }
    }
    if (!has_area(g.settings)) return;
    if (inside(g.settings, s.position)) {
        s.outside_since = 0;
        if (!trainer::telemetry().airborne) s.last_inside = s.position;
        return;
    }
    if (!s.outside_since) s.outside_since = now;
    if (now - s.outside_since >= out_of_area_ms) {
        s.outside_since = now; // try again later if the teleport is refused
        // The last spot inside only while it is inside this game's area (it may be from an earlier game's).
        const auto back = s.last_inside && inside(g.settings, *s.last_inside) ? *s.last_inside : area_centre(g.settings.corners);
        const auto middle = g.settings.area_radius > 0 ? g.settings.corners[0] : area_centre(g.settings.corners);
        // Back inside, facing into the area.
        if (teleport_to(back, std::hypot(middle[0] - back[0], middle[2] - back[2]) > 1.0f ? heading_towards(back, middle) : s.heading))
            popup(s, "Back in the area");
    }
}

// The leader changed the settings before the start: everyone gets them at once.
void settings_changed(Game &g) {
    g.setup_sent = 0;
    // Every Deathrace gate has a facing: one placed without (the console) looks along the route.
    auto &s = g.settings;
    if (s.mode != Mode::race) {
        s.yaws.clear();
        s.widths.clear();
        return;
    }
    for (std::size_t i = s.yaws.size(); i < s.points.size(); ++i) s.yaws.push_back(gate_heading(s, i));
    s.yaws.resize(s.points.size());
    // Every gate has a width too: one placed without keeps the game's checkpoint radius.
    while (s.widths.size() < s.points.size()) s.widths.push_back(std::clamp(s.radius, min_gate_half_width, max_gate_half_width));
    s.widths.resize(s.points.size());
}
bool game_window_focused() {
    DWORD process{};
    const auto window = GetForegroundWindow();
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}
constexpr std::uint32_t pad_up = 0x1, pad_down = 0x2, pad_left = 0x4, pad_right = 0x8;
std::string point_name(const Game &g) { return g.settings.mode == Mode::race ? "checkpoint" : "spot"; }
// A Deathrace route's gates: the first is the start, the last the finish, checkpoints between.
std::string gate_name(std::size_t index, std::size_t count) {
    if (index == 0) return "Start";
    if (index + 1 == count && count >= 2) return "Finish";
    return std::format("Checkpoint {}", index);
}

// The spot the next thing goes, on the real ground: where the free camera looks, or under the
// skater (whose position is carried above the ground, which left Deathrace posts floating). Rays
// are cast on the client update (park_editor_runtime), so the answer is a tick behind.
constexpr std::uint32_t aim_ray = 0x6d6f6401, ground_ray = 0x6d6f6402;
void update_cursor(State &s) {
    if (s.free_place) {
        const auto view = latest_game_view();
        if (!view) {
            s.cursor_ok = false;
            return;
        }
        const auto &m = view->world;
        const Vec3 origin{m[12], m[13], m[14]}, forward{-m[8], -m[9], -m[10]};
        queue_world_ray(aim_ray, origin, forward, 400.0f);
        const auto hit = world_ray(aim_ray);
        s.cursor_ok = hit.hit && now_ms() - hit.when < 500;
        if (s.cursor_ok) s.cursor = hit.at;
        // The gate faces the way the camera looks.
        if (std::hypot(forward[0], forward[2]) > 0.05f) s.cursor_heading = std::atan2(forward[0], forward[2]) * 180.0f / 3.14159265f;
        return;
    }
    queue_world_ray(ground_ray, {s.position[0], s.position[1] + 0.5f, s.position[2]}, {0, -1, 0}, 6.0f);
    const auto hit = world_ray(ground_ray);
    s.cursor = s.position;
    if (hit.hit && now_ms() - hit.when < 500 && std::hypot(hit.at[0] - s.position[0], hit.at[2] - s.position[2]) < 2.0f)
        s.cursor[1] = hit.at[1];
    s.cursor_heading = s.heading;
    s.cursor_ok = true;
}

// Placing: D-pad or keys, read on the game thread while ReSkate's menu is closed.
void run_placing(State &s, std::uint64_t now) {
    if (s.placing != State::Placing::none && (!s.game || !s.game->leading || s.game->referee || (!s.skater && !s.free_place)))
        s.placing = State::Placing::none;
    // While placing, the D-pad is ours: skate. would open its replay editor and quick menus. It
    // stays ours after placing ends until it is let go: the D-pad Down that drops the finish must
    // not reach skate. still held (its replay editor takes the camera).
    constexpr std::uint32_t dpad = pad_up | pad_down | pad_left | pad_right;
    if (s.placing != State::Placing::none) {
        s.dpad_release = true;
    } else if (s.dpad_release) {
        ControllerInput pad;
        DingoSDKOverlayReadControllerInput(&pad);
        if (!pad.available || !(pad.buttons & dpad)) s.dpad_release = false;
    }
    overlay::hide_game_buttons(s.placing != State::Placing::none || s.dpad_release ? dpad : 0);
    // Placing with the free camera, like skate.'s quick drop: on while placing, off after.
    const bool want_freecam = s.placing != State::Placing::none && s.free_place;
    // The sticks fly the camera, so the skater does not roll off meanwhile.
    overlay::pause_game_input(want_freecam || seeker_blind(s));
    // Back to the skater: asked again for a moment in case a request is dropped while busy.
    if (!s.freecam_ours && now < s.freecam_off_until && now - s.freecam_off_sent >= 400) {
        s.freecam_off_sent = now;
        console::request_debug(overlay::DebugAction::set_free_camera, false);
    }
    if (want_freecam != s.freecam_ours) {
        s.freecam_ours = want_freecam;
        console::request_debug(overlay::DebugAction::set_free_camera, want_freecam);
        s.freecam_off_until = want_freecam ? 0 : now + 1600;
        s.freecam_off_sent = now;
        if (want_freecam) {
            const bool route = s.game && s.game->settings.mode == Mode::race && s.placing == State::Placing::points;
            notice(s, "Placing with the free camera. Controller: left stick moves, right stick looks, triggers go down/up, "
                      "click the left stick for speed. Keyboard: WASD, Q/E, hold right mouse to look, Shift. Aim the white reticle at the ground.");
            notice(s, route ? "Left click (or Enter): drop the start, then each checkpoint. F (or End): drop the finish. "
                              "Mouse wheel (or PgUp/PgDn): turn the gate. Backspace: undo. The D-pad works too."
                            : s.placing == State::Placing::circle
                                ? "Mouse wheel (or PgUp/PgDn): size the circle. Left click (or Enter): set it. Backspace: cancel."
                                : "Left click (or Enter): drop one. F (or End): done. Backspace: undo.");
        }
    }
    if (s.placing == State::Placing::none) return;
    update_cursor(s);
    auto &g = *s.game;
    const float seconds = s.placing_tick && now > s.placing_tick ? std::min(0.1f, (now - s.placing_tick) / 1000.0f) : 0.0f;
    s.placing_tick = now;
    ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad);
    const auto buttons = pad.available ? pad.buttons : 0u;
    auto pressed = buttons & ~s.pad_previous;
    s.pad_previous = buttons;
    const auto held = [](int key) { return overlay::key_down(key); }; // past the pause while the free camera flies
    // Mouse and keyboard work as well as the D-pad: with the free camera, left click drops (the
    // right button is the camera's look) and F finishes; the wheel turns a gate or sizes a circle.
    const bool mouse = s.free_place;
    const std::array<bool, 3> keys{held(VK_RETURN) || (mouse && held(VK_LBUTTON)), held(VK_BACK), held(VK_END) || (mouse && held('F'))};
    const int wheel = overlay::take_mouse_wheel(); // only gathered while the free camera flies
    const float notches = static_cast<float>(wheel) / WHEEL_DELTA;
    std::array<bool, 3> struck{};
    for (std::size_t i = 0; i < keys.size(); ++i) struck[i] = keys[i] && !s.keys_previous[i];
    s.keys_previous = keys;
    // Presses meant for ReSkate's menu, the chat or another window do nothing here.
    const bool focus = !overlay::interface_open() && game_window_focused();
    if (!focus) return;
    const bool back = (pressed & pad_left) || struck[1];
    bool confirm = (pressed & pad_right) || struck[0], done = (pressed & pad_down) || struck[2];
    // Nothing goes down while the camera looks at the sky.
    if (!s.cursor_ok && (confirm || (done && s.placing == State::Placing::points && g.settings.mode == Mode::race))) {
        popup(s, "Aim at the ground");
        confirm = done = false;
    }
    if (s.placing == State::Placing::circle) {
        const bool grow = (buttons & pad_up) || held(VK_PRIOR), shrink = (buttons & pad_down) || held(VK_NEXT);
        // Faster the bigger it gets, so a whole plaza is a couple of seconds away.
        const float rate = 6.0f + s.draft_radius * 0.6f;
        if (grow != shrink) s.draft_radius += (grow ? rate : -rate) * seconds;
        s.draft_radius *= std::pow(1.12f, notches); // each wheel notch: 12% bigger or smaller
        s.draft_radius = std::clamp(s.draft_radius, min_area_radius, max_area_radius);
        if (confirm) {
            g.settings.corners = {s.cursor};
            g.settings.area_radius = std::round(s.draft_radius);
            settings_changed(g);
            s.placing = State::Placing::none;
            notice(s, std::format("Area set: a circle {:.0f} m across.", g.settings.area_radius * 2));
        } else if (back) {
            s.placing = State::Placing::none;
        }
        return;
    }
    const bool corners = s.placing == State::Placing::corners;
    auto &list = corners ? g.settings.corners : g.settings.points;
    const auto limit = corners ? max_corners : max_points;
    const bool route = !corners && g.settings.mode == Mode::race;
    if (route) {
        // The gate faces the way the skater is going; holding D-pad Up (or PgUp/PgDn) turns it.
        const bool left = held(VK_PRIOR), right = (buttons & pad_up) || held(VK_NEXT);
        if (left != right) s.yaw_offset += (left ? -120.0f : 120.0f) * seconds;
        // The wheel turns the gate; with Ctrl held (or [ and ]) it widens or narrows it instead.
        const bool ctrl = held(VK_CONTROL);
        if (ctrl) s.draft_width *= std::pow(1.15f, notches);
        else s.yaw_offset += notches * 15.0f; // each wheel notch: 15 degrees
        const bool narrow = held(VK_OEM_4), widen = held(VK_OEM_6);
        if (narrow != widen) s.draft_width *= std::pow(widen ? 1.8f : 1.0f / 1.8f, seconds);
        s.draft_width = std::clamp(s.draft_width, min_gate_half_width, max_gate_half_width);
    }
    const bool finish_now = route && done;
    if (confirm && list.size() + (route ? 1 : 0) < limit) {
        if (route) {
            g.settings.yaws.resize(list.size());
            g.settings.yaws.push_back(s.cursor_heading + s.yaw_offset);
            g.settings.widths.resize(list.size(), s.draft_width);
            g.settings.widths.push_back(s.draft_width); // the next gate starts as wide as this one
            s.yaw_offset = 0;
        }
        list.push_back(s.cursor);
        settings_changed(g);
        popup(s, route ? std::format("{} placed", gate_name(list.size() - 1, list.size() + 1))
                       : std::format("{} {} placed", corners ? std::string("Corner") : point_name(g), list.size()));
    } else if (back) {
        if (list.empty()) {
            s.placing = State::Placing::none;
        } else {
            list.pop_back();
            if (!corners && g.settings.yaws.size() > list.size()) g.settings.yaws.resize(list.size());
            if (!corners && g.settings.widths.size() > list.size()) g.settings.widths.resize(list.size());
            settings_changed(g);
            popup(s, std::format("Removed the last one ({} left)", list.size()));
        }
    } else if (finish_now && !list.empty() && list.size() < limit) {
        // The finish goes where the skater stands.
        g.settings.yaws.resize(list.size());
        g.settings.yaws.push_back(s.cursor_heading + s.yaw_offset);
        g.settings.widths.resize(list.size(), s.draft_width);
        g.settings.widths.push_back(s.draft_width);
        s.yaw_offset = 0;
        list.push_back(s.cursor);
        settings_changed(g);
        s.placing = State::Placing::none;
        notice(s, std::format("Route set: start, {} checkpoint{}, finish.", list.size() - 2, list.size() == 3 ? "" : "s"));
    } else if (done && !route && (corners ? list.size() >= 3 : !list.empty())) {
        s.placing = State::Placing::none;
        notice(s, corners ? std::format("Area set: {} corners.", list.size()) : std::format("{} {}s placed.", list.size(), point_name(g)));
    }
}

// ---- the game ------------------------------------------------------------------------
// A game's final standings onto the lobby leaderboard, once per game, from whichever copy of its
// state arrives first; a solo game counts for nothing. A new leader is called out in chat.
void record_result(State &s, std::uint64_t leader, std::uint32_t game, Mode mode, const Message &m) {
    if (m.phase != Phase::results || m.standings.size() < 2 || !s.lobby_counted.insert({leader, game}).second) return;
    for (std::size_t i = 0; i < m.standings.size(); ++i) {
        auto &e = s.lobby[m.standings[i].player];
        e.name = name_of(s, m.standings[i].player);
        ++e.games;
        if (i == 0) {
            ++e.wins;
            ++e.mode_wins[mode];
            e.best_streak = std::max(e.best_streak, ++e.streak);
        } else {
            e.streak = 0;
        }
        if (i < 3) {
            ++e.podiums;
            e.points += 3 - static_cast<int>(i);
        }
    }
    ++s.lobby_played[mode];
    s.lobby_recent.push_front({mode, name_of(s, m.standings[0].player), name_of(s, m.standings[1].player),
                               static_cast<int>(m.standings.size()), now_ms()});
    if (s.lobby_recent.size() > 6) s.lobby_recent.pop_back();
    const auto top = std::max_element(s.lobby.begin(), s.lobby.end(), [](const auto &a, const auto &b) {
        return std::tie(a.second.points, a.second.wins) < std::tie(b.second.points, b.second.wins);
    });
    if (top != s.lobby.end() && top->first != s.lobby_leader) {
        s.lobby_leader = top->first;
        notice(s, std::format("{} takes the lobby lead: {} point{}, {} win{}!", top->second.name, top->second.points,
                              top->second.points == 1 ? "" : "s", top->second.wins, top->second.wins == 1 ? "" : "s"));
    }
}

void run_leader(State &s, Game &g, std::uint64_t now) {
    if (!g.referee) {
        // Setting up: players who left are dropped from the ones who answered.
        if (s.session) std::erase_if(g.joiners, [&](std::uint64_t id) { return !s.present.contains(id); });
        if (now - g.setup_sent >= setup_every_ms) {
            auto m = header(g, Message::Kind::setup);
            m.settings = g.settings;
            send(s, m);
            g.setup_sent = now;
        }
        g.state = setup_state(s, g);
        if (now - g.state_sent >= state_every_ms) {
            send(s, *g.state);
            g.state_sent = now;
        }
        return;
    }
    auto &r = *g.referee;
    r.set_name(self_id(s), name_of(s, self_id(s)));
    if (g.state)
        for (const auto &p : g.state->standings) {
            if (p.player == self_id(s)) continue;
            r.set_name(p.player, name_of(s, p.player));
            if (s.session && !s.present.contains(p.player)) r.remove_player(p.player, now);
        }
    const bool changed = r.tick(now);
    (void)r.take_calls(); // everyone reads them from the state, the leader too
    if (r.finished(now)) {
        send(s, header(g, Message::Kind::end));
        end_game(s, std::string(mode_name(g.settings.mode)) + " is over.");
        return;
    }
    if (now - g.setup_sent >= setup_every_ms) {
        auto m = header(g, Message::Kind::setup);
        m.settings = g.settings;
        send(s, m);
        g.setup_sent = now;
    }
    // Graffiti: new tag shapes go out at once, and all of them again every few seconds.
    g.tags = r.tags();
    if (g.tags.size() > g.tags_sent || (!g.tags.empty() && now - g.tags_resent >= setup_every_ms)) {
        const bool all = now - g.tags_resent >= setup_every_ms;
        for (std::size_t first = all ? 0 : g.tags_sent; first < g.tags.size(); first += 12) {
            auto m = header(g, Message::Kind::tags);
            m.first = static_cast<std::uint8_t>(first);
            m.tags.assign(g.tags.begin() + static_cast<std::ptrdiff_t>(first),
                          g.tags.begin() + static_cast<std::ptrdiff_t>(std::min(first + 12, g.tags.size())));
            send(s, m);
        }
        if (all) g.tags_resent = now;
        g.tags_sent = g.tags.size();
    }
    g.state = r.state(now);
    record_result(s, g.leader, g.id, g.settings.mode, *g.state);
    if (changed || now - g.state_sent >= state_every_ms) {
        send(s, *g.state);
        g.state_sent = now;
    }
}
void run_player(State &s, Game &g, std::uint64_t now) {
    if (now - g.heard > leader_timeout_ms || (s.session && !s.present.contains(g.leader))) {
        end_game(s, name_of(s, g.leader) + "'s game ended: they are gone.");
        return;
    }
    // Answer the setup until the leader lists us; a game already under way is watched.
    const bool before_start = !g.state || g.state->phase == Phase::setup || g.state->phase == Phase::countdown;
    if (!standing(g, self_id(s)) && before_start && !s.barred && now - g.join_sent >= join_every_ms) {
        send(s, header(g, Message::Kind::join));
        g.join_sent = now;
    }
}

// S.K.A.T.E.: the local player's turn. skate.'s trick list is read from the moment it is theirs: each
// trick's name (with its FS / BS side) joins the attempt until the line lands, which sends it, or a
// bail, which misses it. One attempt per turn: the next turn (a new call from the referee) starts
// over. Whoever is up is watched with the party Spectate camera, when the player wants that.
std::string trick_kinds_text(std::uint8_t kinds) {
    if (kinds == all_trick_kinds) return "any trick";
    std::string text;
    for (const auto &[bit, name] : {std::pair{trick_flips, "flip tricks"}, std::pair{trick_grabs, "grabs"},
                                    std::pair{trick_grinds, "grinds & slides"}, std::pair{trick_manuals, "manuals"}})
        if (kinds & bit) text += (text.empty() ? "" : ", ") + std::string(name);
    return text;
}
std::string skate_trick(const State::SkateTurn &t) {
    std::string text;
    for (const auto &[entry, name] : t.parts) {
        if (!text.empty()) text += " + ";
        if (const auto side = t.sides.find(entry); side != t.sides.end()) text += side->second + " ";
        text += name;
    }
    if (text.size() > max_trick_length) text.resize(max_trick_length);
    return text;
}
void run_skate(State &s, std::uint64_t now) {
    auto &t = s.skate;
    const auto self = self_id(s);
    const auto *st = s.game && s.game->state ? &*s.game->state : nullptr;
    const auto *me = s.game ? standing(*s.game, self) : nullptr;
    const bool in = st && s.game->settings.mode == Mode::skate && st->phase == Phase::playing && me && !me->out;
    if (s.game && s.game->settings.mode == Mode::skate) prepare_trick_gestures(); // once, in the background
    trick_feed().on.store(in, std::memory_order_release);
    auto notes = take_trick_notes();
    // The camera on whoever is up, not on the local player's own turn.
    const auto watch = in && s.spectate && st->turn && st->turn != self && s.present.contains(st->turn) ? st->turn : 0;
    if (watch || t.spectating) multiplayer::spectate_party_member(watch);
    t.spectating = watch;
    const bool mine = in && st->turn == self;
    if (!mine) {
        t = {.spectating = t.spectating};
        return;
    }
    if (!t.mine || t.key != st->call_serial) { // a new turn: nothing done before it counts
        logging::log(logging::Level::info, logging::Channel::runtime, "S.K.A.T.E.: your turn, {} ({} trick list note{} before it ignored).",
                     st->trick.empty() ? std::string("setting") : "copying the " + st->trick, notes.size(), notes.size() == 1 ? "" : "s");
        t.mine = true;
        t.key = st->call_serial;
        t.sent = false;
        t.since = now;
        t.parts.clear();
        t.sides.clear();
        return;
    }
    if (t.sent) return;
    const auto send_attempt = [&](bool landed) {
        const auto trick = skate_trick(t);
        logging::log(logging::Level::info, logging::Channel::runtime, "S.K.A.T.E.: attempt \"{}\" {}.", trick,
                     landed ? "landed" : "missed (a bail or a failed landing)");
        report(s, Event::trick, landed ? 1 : 0, 0, s.position, {}, trick);
        popup(s, landed ? "Landed: " + trick : trick.empty() ? std::string("Missed") : "Missed: " + trick);
        t.sent = true;
    };
    // Reverts count for nothing: a kickflip landed into a revert is a kickflip, and copies one landed without.
    const auto revert = [](const std::string &name) {
        std::string text;
        for (const char c : name) text += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return text.find("revert") != std::string::npos;
    };
    for (const auto &note : notes) {
        logging::log(logging::Level::info, logging::Channel::runtime, "S.K.A.T.E.: trick list {} \"{}\" (entry {:x}).",
                     note.kind == 'n' ? "name" : note.kind == 's' ? "side" : "landing", note.text, note.entry);
        if (note.kind == 'n') {
            if (revert(note.text)) continue;
            if (t.parts.size() < 8) t.parts.push_back({note.entry, note.text});
        } else if (note.kind == 's') {
            t.sides[note.entry] = note.text == "ID_TRICK_FS" ? "FS" : note.text == "ID_TRICK_BS" ? "BS" : std::string();
            if (t.sides[note.entry].empty()) t.sides.erase(note.entry);
        } else if (!t.parts.empty()) { // landed
            send_attempt(note.text.find("BAIL") == std::string::npos && note.text.find("FAIL") == std::string::npos);
            return;
        }
    }
    // A bail during the turn misses it.
    if (s.bail.active && s.bail.started >= t.since) send_attempt(false);
}

// S.K.A.T.E.'s trick to copy for the HUD: each part with its flick (or what kind of trick it is).
void fill_trick_parts(overlay::ModesHud &h, const std::string &trick, std::string title, bool goofy) {
    h.trick_title = std::move(title);
    h.goofy = goofy;
    std::size_t start = 0;
    while (start < trick.size() && h.trick_parts.size() < 4) {
        auto end = trick.find(" + ", start);
        if (end == std::string::npos) end = trick.size();
        overlay::ModesHudTrickPart part;
        part.name = trick.substr(start, end - start);
        part.path = trick_gesture(part.name);
        if (part.path.empty()) {
            const auto kinds = trick_kinds_of(part.name);
            part.kind = kinds & trick_grinds ? "GRIND" : kinds & trick_manuals ? "MANUAL" : kinds & trick_grabs ? "GRAB" : "TRICK";
        }
        h.trick_parts.push_back(std::move(part));
        start = end + 3;
    }
}

// The local player's game for skate.'s own HUD widgets: the intro, the 3-2-1, the score block (the
// leader's value by the crown, the local player's under it, every player's row) with its clock,
// and the results board.
NativeMatch native_snapshot(State &s) {
    NativeMatch n;
    if (!s.game || !s.game->state) return n;
    const auto &g = *s.game;
    const auto &st = *g.state;
    const auto self = self_id(s);
    const auto *me = standing(g, self);
    if (!me && !g.leading) return n; // watching someone else's game: their HUD is theirs
    const auto mode = g.settings.mode;
    n.active = true;
    n.key = game_key(g);
    n.phase = static_cast<std::uint8_t>(st.phase);
    n.remaining_ms = st.remaining_ms;
    n.countdown_ms = countdown_ms;
    n.clock_ms = timed(mode) ? g.settings.duration_s * 1000 : 0;
    n.leading = g.leading;
    n.players = st.standings.size();
    std::string title(mode_name(mode));
    for (auto &c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    n.title = title;
    n.tagline = std::string(mode_tagline(mode));
    if (n.tagline.empty()) n.tagline = std::string(mode_summary(mode));
    // The headline over the score block: the mode, and whose turn it is or who is it.
    n.headline = title;
    if (st.turn && (mode == Mode::one_up || mode == Mode::skate || mode == Mode::tag)) {
        std::string who = st.turn == self ? std::string("YOU") : name_of(s, st.turn);
        for (auto &c : who) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        n.headline += mode == Mode::tag ? "  /  " + who + (st.turn == self ? " ARE IT" : " IS IT") : "  /  " + who;
    }
    const auto value_of = [&](const Standing &p) -> std::string {
        switch (mode) {
        case Mode::jam:
        case Mode::meat:
        case Mode::domination: return grouped(p.score);
        case Mode::one_up: return grouped(p.score);
        case Mode::race:
            return static_cast<std::size_t>(p.score) >= g.settings.points.size() && p.aux ? std::format("{:.1f}s", p.aux / 1000.0)
                                                                                         : std::format("{}/{}", p.score, g.settings.points.size());
        case Mode::graffiti: return std::to_string(p.score);
        case Mode::tag:
        case Mode::infection:
        case Mode::hide: return std::format("{:.1f}s", p.score / 10.0);
        case Mode::skate: return p.aux > 0 ? skate_letters(static_cast<unsigned>(p.aux), g.settings.strikes) : std::string("-");
        }
        return grouped(p.score);
    };
    for (const auto &p : st.standings) {
        NativeMatch::Row row{p.player, name_of(s, p.player), st.phase == Phase::setup ? std::string("READY") : value_of(p),
                             p.player == self, p.up || (st.turn && p.player == st.turn), p.out};
        n.rows.push_back(std::move(row));
    }
    if (!n.rows.empty()) n.best = n.rows.front().value;
    if (me) n.mine = value_of(*me);
    if (st.phase == Phase::results && !st.standings.empty()) n.winner = name_of(s, st.standings.front().player);
    return n;
}

void build_hud(State &s, std::uint64_t now) {
    overlay::ModesHud h;
    if (s.game) {
        const auto &g = *s.game;
        const auto *st = g.state ? &*g.state : nullptr;
        const auto mode = g.settings.mode;
        const auto self = self_id(s);
        const auto *me = standing(g, self);
        h.active = true;
        h.title = std::string(mode_name(mode));
        if (!g.leading) h.title += "  -  " + name_of(s, g.leader) + "'s game";
        const auto phase = st ? st->phase : Phase::setup;
        switch (phase) {
        case Phase::setup:
            if (g.leading) {
                const auto why = missing(g.settings);
                h.status = why.empty() ? std::format("Ready with {} player{}: `mode start` to begin.", st ? st->standings.size() : 1,
                                                     st && st->standings.size() == 1 ? "" : "s")
                                       : why;
            } else {
                h.status = me ? name_of(s, g.leader) + " is setting up. You're in!"
                              : s.barred ? std::string("Your mods change trick scoring: you can watch, not play.")
                                         : std::string("Joining...");
            }
            break;
        case Phase::countdown:
            h.clock = std::to_string((st->remaining_ms + 999) / 1000);
            h.status = std::string(mode_summary(mode));
            break;
        case Phase::playing:
            h.clock = clock_text(st->remaining_ms);
            if (mode == Mode::one_up) {
                const auto target = st->target > 0 ? "beat " + grouped(st->target) : std::string("set a score");
                h.status = st->turn == self ? "YOUR TURN: " + target : name_of(s, st->turn) + " is up: " + target;
            } else if (mode == Mode::skate && st->turn) {
                const bool setting = st->trick.empty();
                // The trick to copy, drawn the way skate.'s own S.K.A.T.E. shows it: each part's flick.
                if (!setting) fill_trick_parts(h, st->trick, st->turn == self ? "COPY THIS" : name_of(s, st->turn) + " IS TRYING", s.goofy);
                if (st->turn == self)
                    h.status = setting ? "YOUR SET: land a trick (" + trick_kinds_text(g.settings.trick_kinds) + ")"
                                       : "YOUR TURN: land the " + st->trick;
                else
                    h.status = setting ? name_of(s, st->turn) + " is setting a trick" : name_of(s, st->turn) + " is trying the " + st->trick;
            } else if (mode == Mode::tag && st->turn) {
                h.status = st->turn == self ? std::string("YOU'RE IT! Get close to someone to tag them")
                                            : name_of(s, st->turn) + " is it. Don't get tagged!";
            } else if (mode == Mode::infection && me) {
                const auto clean = std::count_if(st->standings.begin(), st->standings.end(), [](const Standing &p) { return !p.up && !p.out; });
                h.status = me->up ? std::format("YOU'RE INFECTED! Catch the survivors ({} left)", clean)
                                  : std::format("SURVIVE! Stay away from the reapers ({} still clean)", clean);
            } else if (mode == Mode::hide && me) {
                const auto hiding = std::count_if(st->standings.begin(), st->standings.end(), [](const Standing &p) { return !p.up && !p.out; });
                const auto counting = (std::max(st->target, 0) + 999) / 1000;
                if (st->target > 0)
                    h.status = me->up ? std::format("YOU'RE SEEKING. Count... the hunt starts in {} s", counting)
                                      : std::format("HIDE! The seekers come out in {} s", counting);
                else
                    h.status = me->up ? std::format("FIND THEM! {} still hiding. Follow the heat", hiding)
                                      : std::format("STAY HIDDEN! {} still hiding", hiding);
                if (st->target > 0 && me->up && !me->out) h.blind = std::to_string(counting);
            } else if (!me) {
                h.status = "Watching.";
            } else {
                h.status = std::string(mode_summary(mode));
            }
            break;
        case Phase::results:
            h.clock = "FINAL";
            h.status = st && !st->standings.empty() ? name_of(s, st->standings.front().player) + " wins!" : "Game over.";
            break;
        }
        if (mode == Mode::skate) { // no trick lines in S.K.A.T.E.: the attempt as it is done, then how it went
            if (phase == Phase::playing && s.skate.mine && !s.skate.sent && !s.skate.parts.empty()) h.line = skate_trick(s.skate);
            else if (!s.popup.empty() && now - s.popup_at < popup_ms && me && phase == Phase::playing) h.line = s.popup;
        } else if (s.line.tricks && me && phase == Phase::playing)
            h.line = std::format("{} trick line  -  {}", s.line.tricks, grouped(line_score(s.line.sum, s.line.tricks)));
        else if (!s.popup.empty() && now - s.popup_at < popup_ms && me && phase == Phase::playing)
            h.line = s.popup;
        if (st && st->call_serial && !st->calls.empty()) {
            h.banner = st->calls.back();
            h.banner_serial = st->call_serial;
        }
        // Skate Tag: every player whose position is fresh, the one who is it marked. Infection: the
        // infected marked (all of them hunt).
        if (tag_like(mode) && st && (phase == Phase::playing || phase == Phase::results)) {
            h.infection = mode == Mode::infection;
            h.hide = mode == Mode::hide;
            for (const auto &p : st->standings) {
                const auto found = s.positions.find(p.player);
                if (found == s.positions.end() || now - found->second.second > 3000 || p.out) continue;
                if (mode == Mode::hide && !p.up && phase == Phase::playing) continue; // hiders stay hidden
                const bool marked = hunt_like(mode) ? p.up : p.player == st->turn;
                h.players.push_back({name_of(s, p.player), found->second.first, player_colour(st, p.player), marked, p.player == self});
            }
        }
        // Hide & Seek's hot/cold meter, once the seekers are out: a seeker feels the nearest hider, a
        // hider the nearest seeker. 60 m away or more is freezing.
        if (mode == Mode::hide && me && !me->out && phase == Phase::playing && st->target <= 0) {
            float nearest = -1;
            for (const auto &p : st->standings) {
                if (p.player == self || p.out || p.up == me->up) continue;
                const auto found = s.positions.find(p.player);
                if (found == s.positions.end() || now - found->second.second > 3000) continue;
                const auto &a = found->second.first;
                const float d = std::hypot(a[0] - s.position[0], a[1] - s.position[1], a[2] - s.position[2]);
                if (nearest < 0 || d < nearest) nearest = d;
            }
            if (nearest >= 0) {
                h.heat = std::clamp(1.0f - nearest / 60.0f, 0.0f, 1.0f);
                h.heat_seeking = me->up;
                h.heat_label = h.heat > 0.85f ? "ON FIRE" : h.heat > 0.6f ? "HOT" : h.heat > 0.35f ? "WARM" : h.heat > 0.1f ? "COLD" : "FREEZING";
            }
        }
        // S.K.A.T.E. reads trick names through Hall of Meat's UI hook: without it no attempt can count.
        if (mode == Mode::skate && me && !hall_of_meat::available())
            h.warning = "This game version can't read trick names: S.K.A.T.E. won't see your tricks.";
        if (s.outside_since && phase == Phase::playing) {
            const auto left = out_of_area_ms > now - s.outside_since ? out_of_area_ms - (now - s.outside_since) : 0;
            h.warning = std::format("OUT OF THE AREA: nothing counts. Back in {} s", (left + 999) / 1000);
        }
        if (st)
            for (const auto &p : st->standings) {
                overlay::ModesHudRow row;
                row.name = name_of(s, p.player);
                row.color = player_colour(st, p.player);
                row.self = p.player == self;
                row.up = p.up;
                row.out = p.out;
                switch (mode) {
                case Mode::jam: row.value = grouped(p.score); break;
                case Mode::one_up: {
                    std::string marks;
                    for (unsigned i = 0; i < g.settings.strikes; ++i) marks += static_cast<int>(i) < p.aux ? "X" : "-";
                    row.value = marks + (p.score ? "  " + grouped(p.score) : "");
                    break;
                }
                case Mode::meat: row.value = grouped(p.score) + (p.aux ? "  (best " + grouped(p.aux) + ")" : ""); break;
                case Mode::race:
                    row.value = static_cast<std::size_t>(p.score) >= g.settings.points.size() && p.aux
                                    ? std::format("{:.1f} s", p.aux / 1000.0)
                                    : std::format("{}/{}", p.score, g.settings.points.size());
                    break;
                case Mode::domination: row.value = grouped(p.score) + " pts"; break;
                case Mode::graffiti: row.value = std::format("{} zone{}", p.score, p.score == 1 ? "" : "s"); break;
                case Mode::tag: row.value = std::format("{:.1f} s it", p.score / 10.0); break;
                case Mode::infection: row.value = std::format("{}{:.1f} s", p.up ? "INFECTED  " : "", p.score / 10.0); break;
                case Mode::hide:
                    row.value = p.up ? std::format("SEEKING  {:.1f} s{}", p.score / 10.0, p.aux ? std::format(", found {}", p.aux) : "")
                                     : std::format("{:.1f} s hidden", p.score / 10.0);
                    break;
                case Mode::skate:
                    row.value = p.out ? "OUT" : p.aux > 0 ? skate_letters(static_cast<unsigned>(p.aux), g.settings.strikes) : "no letters";
                    break;
                }
                if (phase == Phase::setup) row.value = "ready";
                h.rows.push_back(std::move(row));
            }
        // The end: who won, big in the middle of everyone's screen, and once in the chat.
        if (phase == Phase::results && st && !st->standings.empty() && !h.rows.empty()) {
            h.results = true;
            h.winner = h.rows.front().name;
            h.winner_value = h.rows.front().value;
            h.winner_color = h.rows.front().color;
            h.closing_ms = st->remaining_ms;
            if (!s.game->announced) {
                s.game->announced = true;
                notice(s, std::format("{} won {} with {}", h.winner, mode_name(mode), h.winner_value));
            }
        }        h.corners = g.settings.corners;
        h.points = g.settings.points;
        h.radius = g.settings.radius;
        h.point_colors.assign(h.points.size(), 0);
        if (mode == Mode::domination && st)
            for (const auto &z : st->zones)
                if (z.zone < h.point_colors.size()) h.point_colors[z.zone] = player_colour(st, z.owner);
        if (mode == Mode::race && me && phase != Phase::results) h.next_point = me->score;
        h.route = mode == Mode::race;
        h.point_yaws = g.settings.yaws;
        h.point_widths = g.settings.widths;
        h.area_radius = g.settings.area_radius;
        h.have_me = s.skater;
        h.me = s.position;
        // Placing: the area or the points follow the cursor (the ground aimed at, or under the skater) until confirmed.
        if (g.leading && s.placing != State::Placing::none) {
            h.placing = true;
            h.aiming = s.free_place;
            h.aim_ok = s.cursor_ok;
            h.cursor = s.cursor;
            const bool m = s.free_place; // mouse and keyboard work too
            switch (s.placing) {
            case State::Placing::circle:
                h.corners = {s.cursor};
                h.area_radius = s.draft_radius;
                h.hint = s.free_place ? "Aim at the centre of your spot" : "Skate to the centre of your spot";
                h.prompts = {{'U', m ? "Wheel up / PgUp" : "PgUp", "Bigger"}, {'D', m ? "Wheel down / PgDn" : "PgDn", "Smaller"}, {'R', m ? "Click / Enter" : "Enter", "Set"}, {'L', "Backspace", "Cancel"}};
                h.line = std::format("{:.0f} m across", s.draft_radius * 2);
                break;
            case State::Placing::corners:
                h.area_radius = 0;
                h.corners.push_back(s.cursor);
                h.hint = s.free_place ? "Aim at each corner of your spot" : "Skate to each corner of your spot";
                h.prompts = {{'R', m ? "Click / Enter" : "Enter", "Add corner"}, {'D', m ? "F / End" : "End", "Done"}, {'L', "Backspace", "Undo"}};
                h.line = std::format("{} corner{} placed{}", g.settings.corners.size(), g.settings.corners.size() == 1 ? "" : "s",
                                     g.settings.corners.size() < 3 ? std::format(" - {} more needed", 3 - g.settings.corners.size()) : "");
                break;
            case State::Placing::points:
                h.points.push_back(s.cursor);
                h.point_colors.push_back(0);
                if (mode == Mode::race) {
                    h.point_yaws.resize(g.settings.points.size());
                    h.point_yaws.push_back(s.cursor_heading + s.yaw_offset);
                    h.point_widths.resize(g.settings.points.size(), g.settings.radius);
                    h.point_widths.push_back(s.draft_width);
                    h.hint = s.free_place ? (g.settings.points.empty() ? "Fly the camera and aim at the start line" : "Aim at the next gate along the course")
                                         : (g.settings.points.empty() ? "Skate to the start line" : "Skate the course");
                    h.prompts = g.settings.points.empty()
                        ? std::vector<overlay::ModesHud::Prompt>{{'R', m ? "Click / Enter" : "Enter", "Start gate"}, {'U', m ? "Mouse wheel" : "PgUp", "Turn"}, {'L', "Backspace", "Cancel"}}
                        : std::vector<overlay::ModesHud::Prompt>{{'R', m ? "Click / Enter" : "Enter", "Checkpoint"}, {'U', m ? "Mouse wheel" : "PgUp", "Turn"}, {'D', m ? "F / End" : "End", "Finish"}, {'L', "Backspace", "Undo"}};
                    h.line = (g.settings.points.empty() ? std::string("Route: nothing yet")
                                                        : std::format("Route: start + {} checkpoint{}", g.settings.points.size() - 1,
                                                                      g.settings.points.size() == 2 ? "" : "s")) +
                             std::format("  -  gate {:.0f} m wide ({})", s.draft_width * 2, m ? "Ctrl + wheel or [ ]" : "[ ]");
                    break;
                }
                h.hint = std::format("{} each {}", s.free_place ? "Aim at" : "Skate to", point_name(g));
                h.prompts = {{'R', m ? "Click / Enter" : "Enter", "Add"}, {'D', m ? "F / End" : "End", "Done"}, {'L', "Backspace", "Undo"}};
                h.line = std::format("{} {}{} placed", g.settings.points.size(), point_name(g), g.settings.points.size() == 1 ? "" : "s");
                break;
            default: break;
            }
        }
        if (mode == Mode::graffiti && st) {
            // Every tag painted in its holder's colour, where it was skated.
            for (const auto &z : st->zones)
                if (z.owner && z.zone < g.tags.size() && !g.tags[z.zone].path.empty())
                    h.tags.push_back({g.tags[z.zone].path, g.tags[z.zone].kind == TagKind::gap, player_colour(st, z.owner)});
        }
        // The area, checkpoints and tags belong to the players in the game: someone watching
        // (they arrived late, or cannot play) sees the scoreboard but not the walls.
        if (!me && !g.leading) {
            h.corners.clear();
            h.area_radius = 0;
            h.points.clear();
            h.point_colors.clear();
            h.next_point = -1;
            h.tags.clear();
        }
    }
    // `mode results`: the results screen with made-up players, to see it without playing a game.
    if (!h.active && now < s.results_preview_until) {
        h.active = h.results = true;
        h.title = "Hall of Meat";
        const std::pair<const char *, std::uint32_t> sample[]{{"Huntredbanzzz", 0xff3c8cffU}, {"skate4leakz", 0xff50d26eU}, {"DingoShen", 0xffff6ad2U},
                                                              {"kickflipkid", 0xff3cd2ffU},   {"bailmaster", 0xffb48cffU}};
        const char *values[]{"48,210", "41,885", "39,402", "22,760", "12,915"};
        for (std::size_t i = 0; i < std::size(sample); ++i) {
            overlay::ModesHudRow row;
            row.name = i == 1 ? name_of(s, self_id(s)) : sample[i].first;
            row.color = sample[i].second;
            row.value = values[i];
            row.self = i == 1;
            h.rows.push_back(std::move(row));
        }
        h.winner = h.rows.front().name;
        h.winner_value = h.rows.front().value;
        h.closing_ms = static_cast<std::uint32_t>(s.results_preview_until - now);
    }
    // `mode diagram <trick>`: S.K.A.T.E.'s flick diagram for a trick, without a game.
    if (!h.active && now < s.diagram_until) {
        h.active = true;
        h.title = "S.K.A.T.E.";
        h.status = "Flick diagram preview";
        fill_trick_parts(h, s.diagram_trick, "COPY THIS", s.goofy);
    }
    // What the Game Modes page of the ReSkate menu shows and offers.
    overlay::ModesMenu m;
    m.bone_cam = bone_cam_setting();
    m.bone_cam_ringing = bone_cam_ringing();
    m.spectate = s.spectate;
    m.in_session = s.session;
    for (const auto id : s.present) {
        const auto heard = s.modded.find(id);
        if (heard == s.modded.end() || now - heard->second > 20000) ++m.lobby_without;
        else if (s.other_version.contains(id)) m.lobby_outdated.push_back(name_of(s, id));
        else m.lobby_modded.push_back(name_of(s, id));
    }
    for (const auto &[id, e] : s.lobby) {
        overlay::ModesLeaderRow row{e.name, e.points, e.wins, e.podiums, e.games, e.streak, e.best_streak, {}, id == self_id(s)};
        int most{};
        for (const auto &[mode, wins] : e.mode_wins)
            if (wins > most) {
                most = wins;
                row.best_mode = std::string(mode_name(mode));
            }
        m.leaders.push_back(std::move(row));
    }
    std::stable_sort(m.leaders.begin(), m.leaders.end(), [](const auto &a, const auto &b) {
        return std::tie(a.points, a.wins, a.podiums) > std::tie(b.points, b.wins, b.podiums);
    });
    for (const auto &g : s.lobby_recent)
        m.recent.push_back({std::string(mode_name(g.mode)), g.winner, g.second, g.players,
                            static_cast<std::uint32_t>((now - g.at) / 1000)});
    m.lobby_games = static_cast<int>(s.lobby_counted.size());
    int played{};
    for (const auto &[mode, count] : s.lobby_played)
        if (count > played) {
            played = count;
            m.lobby_top_mode = std::string(mode_name(mode));
        }
    m.goofy = s.goofy;
    m.official_meat = hall_of_meat::available();
    m.meat_every_bail = hall_of_meat::switched_on();
    if (s.game) {
        const auto &g = *s.game;
        m.in_game = true;
        m.leading = g.leading;
        m.phase = g.state ? static_cast<int>(g.state->phase) : static_cast<int>(Phase::setup);
        m.mode = std::string(mode_key(g.settings.mode));
        m.corners = g.settings.corners.size();
        m.points = g.settings.points.size();
        m.players = g.state ? g.state->standings.size() : 1;
        m.duration = g.settings.duration_s;
        m.turn = g.settings.turn_s;
        m.strikes = g.settings.strikes;
        m.trick_kinds = g.settings.trick_kinds;
        m.radius = g.settings.radius;
        if (g.leading && !g.referee) m.missing = missing(g.settings);
        m.area_radius = g.settings.area_radius;
        m.placing = s.placing == State::Placing::circle ? "circle" : s.placing == State::Placing::corners ? "corners"
                  : s.placing == State::Placing::points ? "points" : "";
    }
    // Other players' games: banners over the world, the announcement, and the menu's list.
    for (const auto &o : s.offers) {
        if (!o.confirmed) continue; // its phase is not known yet (a state message follows within half a second)
        overlay::ModesHudOffer offer;
        std::string mode(mode_name(o.settings.mode));
        for (auto &c : mode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        offer.mode = mode;
        offer.host = name_of(s, o.leader) + (o.leader == s.invite_leader && now - s.invite_at < invite_ms ? "  -  NEW" : "");
        offer.open = offer_open(o);
        offer.detail = std::format("{} player{}  -  {}", o.players, o.players == 1 ? "" : "s",
                                   o.phase == Phase::countdown ? "starting now!" : offer.open ? "join now" : "in progress");
        if (const auto spot = game_spot(o.settings)) {
            offer.at = *spot;
            offer.has_at = true;
        }
        offer.target = s.join_target && s.join_target->first == o.leader && s.join_target->second == o.game;
        offer.id = o.leader;
        h.offers.push_back(offer);
        m.offers.push_back(offer);
    }
    // The local player's own game while it takes players: its flag stands where the others join,
    // so its leader sees what they see. Not in the menu's list: there is nothing to join.
    if (s.game && s.game->leading) {
        const auto &g = *s.game;
        const auto phase = g.state ? g.state->phase : Phase::setup;
        if (const auto spot = game_spot(g.settings); spot && (phase == Phase::setup || phase == Phase::countdown)) {
            overlay::ModesHudOffer own;
            std::string mode(mode_name(g.settings.mode));
            for (auto &c : mode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            own.mode = mode;
            own.host = "YOUR GAME";
            const auto players = g.state ? g.state->standings.size() : 1;
            own.detail = std::format("{} player{}  -  others join at this flag", players, players == 1 ? "" : "s");
            own.at = *spot;
            own.has_at = own.open = own.own = true;
            h.offers.push_back(own);
        }
    }
    h.can_join = s.join_target.has_value();
    if (s.invite_at && now - s.invite_at < invite_ms) {
        for (const auto &o : s.offers)
            if (o.leader == s.invite_leader && o.game == s.invite_game) {
                h.invite = std::format("{} is starting {}", name_of(s, o.leader), mode_name(o.settings.mode));
                const float age = static_cast<float>(now - s.invite_at);
                h.invite_fade = std::clamp(std::min(age / 250.0f, (invite_ms - age) / 1000.0f), 0.0f, 1.0f);
            }
    }
    auto native = native_snapshot(s);
    s.counting_down.store(s.game && s.game->state && s.game->state->phase == Phase::countdown && standing(*s.game, self_id(s)));
    s.flaggable.store(s.game && s.game->leading && (!s.game->state || s.game->state->phase != Phase::results) ? game_key(*s.game) : 0);
    std::lock_guard lock(s.hud_mutex);
    s.hud = std::move(h);
    s.menu = std::move(m);
    s.native = std::move(native);
}

std::optional<float> number(const std::vector<std::string> &arguments, std::size_t index) {
    if (index >= arguments.size()) return std::nullopt;
    float value{};
    const auto &text = arguments[index];
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}
std::string help() {
    std::string text = "Game modes. Start one with `mode new <mode>`:";
    for (const auto m : all_modes) text += std::format("\n  {:<11} {}: {}", mode_key(m), mode_name(m), mode_summary(m));
    text += "\nThen, as its leader: `mode corner` at each corner of the area (undo|clear), `mode point` for checkpoints"
            " or spots (undo|clear), `mode time|turn <seconds>`, `mode strikes <n>`, `mode radius <metres>`,"
            " `mode start`, `mode stop`. Anyone: `mode status`, `mode leave`, `mode bonecam meat|on|off|test`."
            " Or use the GAME MODES page of the ReSkate menu.";
    return text;
}
} // namespace

std::string apply_flag(State &s, const Vec3 &spot, float yaw_degrees);
std::vector<std::vector<std::uint8_t>> tick(const SessionInput &input) {
    auto &s = state();
    const auto now = now_ms();
    // A native flag placed from the menu's thread.
    std::optional<std::pair<Vec3, float>> flag;
    {
        std::lock_guard lock(s.hud_mutex);
        flag = std::exchange(s.pending_flag, std::nullopt);
    }
    if (flag)
        if (auto said = apply_flag(s, flag->first, flag->second); !said.starts_with("error")) notice(s, std::move(said));
    s.session = input.local != 0;
    if (!s.session && !s.lobby_counted.empty()) { // left the lobby: a new one starts a new leaderboard
        s.lobby.clear();
        s.lobby_counted.clear();
        s.lobby_recent.clear();
        s.lobby_played.clear();
        s.lobby_leader = 0;
    }
    s.barred = input.barred;
    if (input.local != s.local || (input.world != s.world && s.game)) {
        if (s.game) end_game(s, "The game ended: the session or map changed.");
        s.sat_out.clear();
    }
    s.local = input.local;
    s.world = input.world;
    s.local_name = input.local_name;
    s.present.clear();
    for (const auto &peer : input.peers) {
        s.present.insert(peer.id);
        if (!peer.name.empty()) s.names[peer.id] = peer.name;
        if (peer.at) s.positions[peer.id] = {*peer.at, now};
    }
    try {
        track_skater(s, input.in_world, now);
        run_placing(s, now);
        run_invites(s, now);
        if (s.game) {
            if (s.game->leading) run_leader(s, *s.game, now);
            else run_player(s, *s.game, now);
        }
        line_up(s);
        track_game(s, now);
        // The ragdoll bounces in Hall of Meat (or always, if the player asked).
        set_bail_bounce(s.bounce_always || local_meat_game() ? s.bounce : 0.0f);
        // Hall of Meat games: ReSkate's Hall of Meat held on while one is played (not saved; the
        // menu's switch is the player's own), and every bail that hurt a bone scored with its Meat.
        const bool official = official_meat();
        hall_of_meat::set_forced(official);
        // S.K.A.T.E. and the trick logger read skate.'s trick list through Hall of Meat's UI model
        // write hook, which the tap keeps on while it is set.
        run_skate(s, now);
        // Everyone with game modes says so every few seconds: the menu lists them, and a game of
        // another version that cannot read it tells its player to update.
        if (s.session && now - s.hello_sent >= 5000) {
            Message hello;
            hello.kind = Message::Kind::hello;
            hello.leader = self_id(s);
            hello.game = 1;
            if (hello.leader) send(s, hello);
            s.hello_sent = now;
        }
        const bool tap = trick_log().on.load(std::memory_order_acquire) || trick_feed().on.load(std::memory_order_acquire);
        static bool tapped = false;
        if (tap != tapped) {
            hall_of_meat::set_model_write_tap(tap ? &trick_tap : nullptr);
            logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: skate.'s trick list {}.",
                         tap ? (hall_of_meat::available() ? "is read from now on" : "cannot be read on this game version") : "is no longer read");
            tapped = tap;
        }
        flush_trick_log();
        if (const auto bail = hall_of_meat::take_finished_bail(); bail && official && bail->shown && bail->meat > 0) {
            report(s, Event::bail, bail->meat, 0, s.position);
            popup(s, "Meat: " + grouped(bail->meat));
        }
        build_hud(s, now);
    } catch (...) {}
    auto out = std::move(s.outbox);
    s.outbox.clear();
    if (!s.session) out.clear();
    return out;
}

bool receive(std::uint64_t sender, std::span<const std::uint8_t> bytes) {
    if (!is_mode_message(bytes)) return false;
    auto &s = state();
    const auto decoded = decode(bytes);
    if (!decoded) {
        // Another version's message (a hello, every few seconds, at the least): say whose, and which of
        // the two has to restart to update.
        if (const auto version = message_version(bytes); version && version != wire_version) {
            s.other_version[sender] = version;
            s.modded[sender] = now_ms();
            if (s.version_told.insert(sender).second) {
                notice(s, version > wire_version
                              ? name_of(s, sender) + " has a newer ReSkate game modes: restart your game so it updates, then you can play together."
                              : name_of(s, sender) + " has an older ReSkate game modes: they need to restart their game so it updates.");
                logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: player {:#x} speaks version {} (this game {}).", sender,
                             version, wire_version);
            }
        }
        return true;
    }
    const auto &m = *decoded;
    const auto now = now_ms();
    if (!s.modded.contains(sender))
        logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: player {:#x} has game modes (version {}).", sender, wire_version);
    s.modded[sender] = now;
    s.other_version.erase(sender);
    if (m.kind == Message::Kind::hello) return true;
    const bool ours = s.game && s.game->leader == m.leader && s.game->id == m.game;
    switch (m.kind) {
    case Message::Kind::setup: {
        if (sender != m.leader || s.sat_out.contains({m.leader, m.game})) break;
        if (ours) {
            if (!s.game->leading && s.game->settings != m.settings) {
                s.game->settings = m.settings;
            }
            s.game->heard = now;
            break;
        }
        // Someone else's game: offered, like a throwdown drop, never joined without asking.
        auto found = std::find_if(s.offers.begin(), s.offers.end(), [&](const Offer &o) { return o.leader == m.leader && o.game == m.game; });
        if (found == s.offers.end()) {
            // A leader's new game replaces their old offer.
            // Announced once its leader's state says it is still taking players.
            std::erase_if(s.offers, [&](const Offer &o) { return o.leader == m.leader; });
            s.offers.push_back({m.leader, m.game, m.settings, now});
        } else {
            found->settings = m.settings;
            found->heard = now;
        }
        break;
    }
    case Message::Kind::state:
        if (ours && !s.game->leading && sender == m.leader) {
            s.game->state = m;
            s.game->heard = now;
            record_result(s, m.leader, m.game, s.game->settings.mode, m);
        } else if (!ours && sender == m.leader) {
            for (auto &o : s.offers)
                if (o.leader == m.leader && o.game == m.game) {
                    record_result(s, o.leader, o.game, o.settings.mode, m); // a game played without us counts too
                    o.phase = m.phase;
                    o.players = std::max<std::size_t>(1, m.standings.size());
                    o.heard = now;
                    // First word of its phase: a game still taking players is announced.
                    if (!std::exchange(o.confirmed, true) && offer_open(o) && !s.game) {
                        s.invite_at = now;
                        s.invite_leader = o.leader;
                        s.invite_game = o.game;
                        play(Cue::invite);
                        notice(s, std::format("{} is starting {}! Hold A / X (or press J) to join, or open GAME MODES.",
                                              name_of(s, o.leader), mode_name(o.settings.mode)));
                    }
                }
        }
        break;
    case Message::Kind::event:
        // Positions are the session's own skater poses (tick), never a player's say-so: a sent one
        // is ignored, so nobody can place themselves somewhere else in Skate Tag.
        if (ours && s.game->referee && m.event != Event::position)
            s.game->referee->event(sender, m.event, m.value, m.at, m.sequence, now, m.tags, m.trick);
        break;
    case Message::Kind::tags:
        // Graffiti tag shapes from the leader, from index `first` on.
        if (ours && !s.game->leading && sender == m.leader) {
            auto &tags = s.game->tags;
            if (tags.size() < m.first + m.tags.size()) tags.resize(m.first + m.tags.size());
            for (std::size_t i = 0; i < m.tags.size(); ++i) tags[m.first + i] = m.tags[i];
            s.game->heard = now;
        }
        break;
    case Message::Kind::join:
        if (ours && s.game->leading && sender != self_id(s)) {
            if (s.game->referee) {
                s.game->referee->add_player(sender); // until the countdown ends
            } else if (std::find(s.game->joiners.begin(), s.game->joiners.end(), sender) == s.game->joiners.end() &&
                       s.game->joiners.size() + 1 < max_players) {
                s.game->joiners.push_back(sender);
            }
        }
        break;
    case Message::Kind::leave:
        if (ours && s.game->leading) {
            if (s.game->referee) s.game->referee->remove_player(sender, now);
            else std::erase(s.game->joiners, sender);
        }
        break;
    case Message::Kind::end:
        if (ours && !s.game->leading && sender == m.leader)
            end_game(s, std::string(mode_name(s.game->settings.mode)) + " is over.");
        else if (!ours && sender == m.leader) { // someone else's game was stopped: no longer offered
            s.sat_out.insert({m.leader, m.game});
            std::erase_if(s.offers, [&](const Offer &o) { return o.leader == m.leader && o.game == m.game; });
        }
        break;
    }
    return true;
}

std::vector<std::string> take_notices() { return std::exchange(state().notices, {}); }

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto &s = state();
    const auto now = now_ms();
    const std::string v(verb);
    if (v.empty() || v == "help" || v == "list") return help();
    if (v == "bonecam") {
        // With ReSkate's own Hall of Meat, `on` / `off` / `meat` are its switch (every bail, or Hall of
        // Meat games only); the Bone Cam keeps the choice for a game build without it.
        const auto word = arguments.empty() ? std::string() : arguments[0];
        if (hall_of_meat::available() && (word == "on" || word == "off" || word == "meat")) {
            (void)bone_cam_command(arguments);
            hall_of_meat::set_enabled(word == "on");
            return word == "on" ? "Hall of Meat shows on every bail." : "Hall of Meat shows in Hall of Meat games.";
        }
        return bone_cam_command(arguments);
    }
    if (v == "games") {
        if (s.offers.empty()) return "No other games right now.";
        std::string text = "Open games (mode join <n>):";
        for (std::size_t i = 0; i < s.offers.size(); ++i) {
            const auto &o = s.offers[i];
            text += std::format("\n  {}. {}'s {} - {} player{}{}", i + 1, name_of(s, o.leader), mode_name(o.settings.mode), o.players,
                                o.players == 1 ? "" : "s", offer_open(o) ? "" : " (in progress)");
        }
        return text;
    }
    if (v == "join") {
        // `mode join` takes the announced or nearest game; `mode join <n>` the list's nth; a menu row
        // passes its host's id.
        const Offer *pick{};
        if (arguments.empty()) {
            for (const auto &o : s.offers)
                if (s.join_target && o.leader == s.join_target->first && o.game == s.join_target->second) pick = &o;
            if (!pick && s.offers.size() == 1) pick = &s.offers.front();
        } else {
            std::uint64_t value{};
            const auto &text = arguments[0];
            if (std::from_chars(text.data(), text.data() + text.size(), value).ec == std::errc{}) {
                if (value >= 1 && value <= s.offers.size()) pick = &s.offers[value - 1];
                for (const auto &o : s.offers)
                    if (o.leader == value) pick = &o;
            }
        }
        if (!pick) return s.offers.empty() ? "error: there is no game to join." : "error: which one? mode games lists them.";
        const auto offer = *pick;
        return join_offer(s, offer, now);
    }
    if (v == "tricklog") return trick_log_command(arguments);
    if (v == "diagram") {
        std::string trick;
        for (const auto &word : arguments) trick += (trick.empty() ? "" : " ") + word;
        if (trick.empty()) return "error: usage: mode diagram <trick>, e.g. mode diagram 360 Flip + Indy";
        prepare_trick_gestures();
        s.diagram_trick = trick;
        s.diagram_until = now + 10000;
        return trick_gesture(trick.substr(0, trick.find(" + "))).empty()
                   ? "Showing it for 10 seconds (if the trick has a flick, its diagram shows once the game's gestures are read)."
                   : "Showing its flick diagram for 10 seconds.";
    }
    if (v == "stance") {
        if (arguments.empty() || (arguments[0] != "regular" && arguments[0] != "goofy"))
            return std::format("S.K.A.T.E.'s flick diagrams are shown for a {} stance. Usage: mode stance regular|goofy", s.goofy ? "goofy" : "regular");
        s.goofy = arguments[0] == "goofy";
        return std::format("Flick diagrams shown for a {} stance.", s.goofy ? "goofy" : "regular");
    }
    if (v == "grid") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off"))
            return std::format("The Throwdowns grid is {}. Usage: mode grid on|off", throwdown_grid_on.load() ? "on" : "off");
        throwdown_grid_on.store(arguments[0] == "on");
        return throwdown_grid_on.load() ? "Throwdowns shows the cards in a grid (reopen Throwdowns)."
                                        : "Throwdowns shows the cards in one row (reopen Throwdowns).";
    }
    if (v == "spectate") {
        if (arguments.empty() || (arguments[0] != "on" && arguments[0] != "off"))
            return std::format("S.K.A.T.E. spectating is {}. Usage: mode spectate on|off", s.spectate ? "on" : "off");
        s.spectate = arguments[0] == "on";
        return s.spectate ? "In S.K.A.T.E. the camera follows whoever is up." : "In S.K.A.T.E. the camera stays on you.";
    }
    if (v == "results") {
        s.results_preview_until = now + 12000;
        return "Showing a sample results screen for 12 seconds.";
    }
    if (v == "bounce") {
        const auto describe = [&] {
            return s.bounce <= 0 ? std::string("Bail bounce off.")
                                 : std::format("Bail bounce {:.2f} ({}).", s.bounce, s.bounce_always ? "every bail" : "in Hall of Meat");
        };
        if (arguments.empty()) return describe() + " Usage: mode bounce <0-1>|off|meat|always.";
        const auto &word = arguments[0];
        if (word == "off") s.bounce = 0;
        else if (word == "meat") s.bounce_always = false;
        else if (word == "always") s.bounce_always = true;
        else if (const auto value = number(arguments, 0); value && *value >= 0 && *value <= 1.5f) s.bounce = *value;
        else return "error: usage: mode bounce <0-1>|off|meat|always";
        if ((word == "meat" || word == "always") && s.bounce <= 0) s.bounce = 0.55f;
        return describe();
    }
    if (v == "debug") {
        const bool on = arguments.size() >= 2 && arguments[0] == "states" && arguments[1] == "on";
        if (arguments.size() < 2 || arguments[0] != "states" || (arguments[1] != "on" && arguments[1] != "off"))
            return "error: usage: mode debug states on|off";
        s.log_states = on;
        s.logged_state = 0;
        return on ? "Logging physics states to ReSkate.log." : "Stopped logging physics states.";
    }
    if (v == "status") {
        if (!s.game) return "No game. " + help();
        const auto &g = *s.game;
        std::string text = std::format("{} led by {}; ", mode_name(g.settings.mode), name_of(s, g.leader));
        text += std::format("area: {} corners, points: {}, ", g.settings.corners.size(), g.settings.points.size());
        text += timed(g.settings.mode) ? std::format("time: {} s", g.settings.duration_s)
                                       : std::format("turns: {} s, strikes: {}", g.settings.turn_s, g.settings.strikes);
        if (g.state) text += std::format(", players: {}", g.state->standings.size());
        return text;
    }
    if (v == "new") {
        const auto mode = arguments.empty() ? std::nullopt : parse_mode(arguments[0]);
        if (!mode) return "error: usage: mode new jam|1up|meat|race|domination|graffiti|tag|skate";
        if (s.barred) return "error: your mods change trick scoring, so you cannot start a game in this session.";
        if (!s.skater) return "error: get on your board in the world first.";
        if (s.game && !s.game->leading && s.game->state && s.game->state->phase != Phase::results)
            return "error: " + name_of(s, s.game->leader) + "'s game is on. `mode leave` first.";
        if (s.game && s.game->leading) send(s, header(*s.game, Message::Kind::end));
        Game g;
        g.leader = self_id(s);
        g.id = static_cast<std::uint32_t>((now * 2654435761ULL) ^ (now >> 16)) | 1u;
        g.leading = true;
        g.settings.mode = *mode;
        g.settings.duration_s = default_duration(*mode);
        if (tag_like(*mode)) g.settings.radius = default_tag_reach; // how close tags (or infects)
        if (*mode == Mode::skate) { // S.K.A.T.E.: five letters, time to line a trick up
            g.settings.strikes = 5;
            g.settings.turn_s = 45;
        }
        // Where it was set up: its banner's spot for the others until an area or route is placed.
        g.settings.spawn = s.position;
        g.settings.has_spawn = true;
        g.heard = now;
        s.game = std::move(g);
        std::string next;
        switch (*mode) {
        case Mode::race: next = "Open GAME MODES and press PLACE ROUTE: drop the start, the checkpoints and the finish as you skate the course."; break;
        case Mode::domination: next = "Ride to each spot and use `mode point`, then `mode start`."; break;
        case Mode::graffiti: next = "Open GAME MODES to set an area if you want one, then START GAME."; break;
        case Mode::tag: next = "The whole map is the playground; PLACE CIRCLE in GAME MODES fences it in (up to 3 km across). Then START GAME."; break;
        case Mode::infection: next = "The whole map is the hunting ground; PLACE CIRCLE in GAME MODES fences it in. Then START GAME."; break;
        case Mode::hide: next = "Pick the hiding time in GAME MODES, PLACE CIRCLE to keep everyone close (recommended), then START GAME."; break;
        case Mode::skate: next = "Pick the tricks that count in GAME MODES (or `mode tricks flips grabs grinds manuals`), then START GAME."; break;
        default: next = "Optional: `mode corner` at each corner of an area. Then `mode start`."; break;
        }
        return std::format("{} set up. {}", mode_name(*mode), next);
    }
    if (!s.game) return "error: no game. Start one with `mode new <mode>`.";
    auto &g = *s.game;
    if (v == "leave") {
        if (g.leading) return "error: you lead this game: `mode stop` ends it for everyone.";
        send(s, header(g, Message::Kind::leave));
        s.sat_out.insert({g.leader, g.id});
        end_game(s, {});
        return "You left the game.";
    }
    if (!g.leading) return "error: only " + name_of(s, g.leader) + " (the leader) can change or start this game.";
    if (v == "stop") {
        // Stopped while it is on: everyone sees the results first.
        if (g.referee && (g.referee->phase() == Phase::playing || g.referee->phase() == Phase::countdown)) {
            g.referee->end_now(now);
            g.state_sent = 0;
            return "Game ended: here are the results.";
        }
        send(s, header(g, Message::Kind::end));
        end_game(s, {});
        return "Game stopped.";
    }
    if (g.referee) return "error: the game has started; only `mode stop` now.";
    if (v == "flag") {
        // skate.'s own Throwdown flag (Spot Battle's placement): where everyone starts.
        s.flaggable.store(game_key(g));
        return multiplayer::one_up::begin_mode_flag_placement(true)
            ? std::string("Place your start flag: skate.'s flag placement is opening.")
            : std::string("error: the flag can't be placed right now (a 1-Up or another flag is up, or no world is loaded).");
    }
    const auto changed = [&](std::string text) {
        settings_changed(g);
        return text;
    };
    if (v == "place") {
        const auto what = arguments.empty() ? std::string("circle") : arguments[0];
        if (what == "cancel" || what == "done") {
            s.placing = State::Placing::none;
            return "Placing stopped.";
        }
        if (what == "freecam") {
            const auto value = arguments.size() > 1 ? arguments[1] : std::string(s.free_place ? "off" : "on");
            if (value != "on" && value != "off") return "error: usage: mode place freecam on|off";
            s.free_place = value == "on";
            return s.free_place ? "Placing flies the free camera: aim at the ground (WASD/mouse or the sticks) and drop with D-pad Right (Enter)."
                                : "Placing follows your skater: skate to each spot.";
        }
        if (!s.skater) return "error: get on your board in the world first.";
        // Presses already down when placing starts (the menu's own) count only once let go.
        s.pad_previous = ~0u;
        s.keys_previous = {true, true, true};
        s.placing_tick = 0;
        if (what == "circle") {
            if (g.settings.area_radius > 0) s.draft_radius = g.settings.area_radius;
            s.placing = State::Placing::circle;
            return "Close the menu and skate to the centre of your spot. D-pad Up/Down (PgUp/PgDn) sizes the circle; "
                   "D-pad Right (Enter) sets it.";
        }
        if (what == "corners") {
            g.settings.corners.clear();
            g.settings.area_radius = 0;
            settings_changed(g);
            s.placing = State::Placing::corners;
            return "Close the menu and skate to each corner: D-pad Right (Enter) adds one, D-pad Down (End) finishes.";
        }
        if (what == "points") {
            if (g.settings.mode == Mode::race) { // a route is skated again from its start (a native flag's stays)
                const bool flagged = s.flag_key == game_key(g) && !g.settings.points.empty();
                g.settings.points.resize(flagged ? 1 : 0);
                g.settings.yaws.resize(flagged && !g.settings.yaws.empty() ? 1 : 0);
                g.settings.widths.resize(flagged && !g.settings.widths.empty() ? 1 : 0);
                s.yaw_offset = 0;
                s.draft_width = std::clamp(g.settings.radius, min_gate_half_width, max_gate_half_width);
                settings_changed(g);
            }
            s.placing = State::Placing::points;
            return g.settings.mode == Mode::race
                ? std::string("Close the menu and skate the course: D-pad Right (Enter) drops the start and each checkpoint, D-pad Down (End) the finish.")
                : std::format("Close the menu and skate to each {}: D-pad Right (Enter) adds one, D-pad Down (End) finishes.", point_name(g));
        }
        return "error: usage: mode place circle|corners|points|cancel";
    }
    if (v == "circle") {
        if (!s.skater) return "error: get on your board in the world first.";
        const auto radius = arguments.empty() ? std::optional(s.draft_radius) : number(arguments, 0);
        if (!radius || *radius < min_area_radius || *radius > max_area_radius)
            return std::format("error: usage: mode circle [radius {}-{}]", min_area_radius, max_area_radius);
        s.draft_radius = *radius;
        g.settings.corners = {s.position};
        g.settings.area_radius = std::round(*radius);
        return changed(std::format("Area set: a circle {:.0f} m across, around you.", g.settings.area_radius * 2));
    }
    if (v == "corner" || v == "point") {
        auto &list = v == "corner" ? g.settings.corners : g.settings.points;
        const auto limit = v == "corner" ? max_corners : max_points;
        const auto what = std::string(v == "corner" ? "corner" : point_name(g));
        const auto sub = arguments.empty() ? std::string() : arguments[0];
        if (sub == "clear") {
            list.clear();
            if (v == "corner") g.settings.area_radius = 0;
            return changed(std::format("All {}s cleared.", what));
        }
        if (sub == "undo") {
            if (list.empty()) return "error: nothing to undo.";
            list.pop_back();
            if (g.settings.corners.empty()) g.settings.area_radius = 0;
            return changed(std::format("Last {} removed ({} left).", what, list.size()));
        }
        if (!s.skater) return "error: no skater to mark the spot with.";
        if (v == "corner" && g.settings.area_radius > 0) { // corners replace a circle
            g.settings.corners.clear();
            g.settings.area_radius = 0;
        }
        if (list.size() >= limit) return std::format("error: at most {} {}s.", limit, what);
        list.push_back(s.position);
        return changed(std::format("{} {} placed at {:.1f}, {:.1f}, {:.1f}.", what, list.size(), s.position[0], s.position[1],
                                   s.position[2]));
    }
    const auto set_number = [&](float low, float high, auto apply, std::string_view label) -> std::string {
        const auto value = number(arguments, 0);
        if (!value || *value < low || *value > high) return std::format("error: usage: mode {} <{}-{}>", v, low, high);
        apply(*value);
        return changed(std::format("{} set to {}.", label, *value));
    };
    if (v == "time") return set_number(30, 3600, [&](float x) { g.settings.duration_s = static_cast<std::uint32_t>(x); }, "Time (s)");
    if (v == "turn") return set_number(10, 120, [&](float x) { g.settings.turn_s = static_cast<std::uint32_t>(x); }, "Turn time (s)");
    if (v == "strikes") return set_number(1, 5, [&](float x) { g.settings.strikes = static_cast<std::uint8_t>(x); }, "Strikes");
    if (v == "tricks") {
        // S.K.A.T.E.: the kinds of trick that may be set, one or more.
        std::uint8_t kinds = 0;
        for (const auto &word : arguments) {
            if (word == "flips" || word == "flip") kinds |= trick_flips;
            else if (word == "grabs" || word == "grab") kinds |= trick_grabs;
            else if (word == "grinds" || word == "grind" || word == "slides") kinds |= trick_grinds;
            else if (word == "manuals" || word == "manual") kinds |= trick_manuals;
            else if (word == "all" || word == "any") kinds = all_trick_kinds;
            else return "error: usage: mode tricks flips|grabs|grinds|manuals|all (one or more)";
        }
        if (!kinds) return "error: usage: mode tricks flips|grabs|grinds|manuals|all (one or more)";
        g.settings.trick_kinds = kinds;
        return changed("Tricks that can be set: " + trick_kinds_text(kinds) + ".");
    }
    if (v == "radius")
        return set_number(1, 50, [&](float x) {
            g.settings.radius = x;
            // Deathrace: the reach is how wide its gates are, the ones placed already too.
            const float half = std::clamp(x, min_gate_half_width, max_gate_half_width);
            for (auto &width : g.settings.widths) width = half;
            s.draft_width = half;
        }, "Radius (m)");
    if (v == "start") {
        if (const auto why = missing(g.settings); !why.empty()) return "error: " + why;
        // Started from a native flag: everyone gathers at the flag. Else where the leader stands.
        if (s.skater && s.flag_key != game_key(g)) {
            g.settings.spawn = s.position;
            g.settings.has_spawn = true;
            settings_changed(g);
        }
        g.referee.emplace(g.settings, g.leader, g.id);
        g.referee->add_player(self_id(s));
        for (const auto id : g.joiners) g.referee->add_player(id);
        g.referee->start(now);
        g.setup_sent = g.state_sent = 0;
        return std::format("{} starting with {} player{}!", mode_name(g.settings.mode), g.joiners.size() + 1,
                           g.joiners.empty() ? "" : "s");
    }
    return "error: unknown `mode " + v + "`. " + help();
}

overlay::ModesMenu menu_view() {
    auto &s = state();
    std::lock_guard lock(s.hud_mutex);
    return s.menu;
}

bool infected_look(std::uint64_t player) noexcept {
    const auto &s = state();
    if (!s.game || !s.game->state || !hunt_like(s.game->settings.mode)) return false; // infected, or seeking
    const auto phase = s.game->state->phase;
    if (phase != Phase::playing && phase != Phase::results) return false;
    const auto *p = standing(*s.game, player);
    return p && p->up && !p->out;
}
bool throwdown_grid() noexcept { return throwdown_grid_on.load(); }
bool hidden_player(std::uint64_t player) noexcept {
    const auto &s = state();
    if (!s.game || !playing(*s.game) || s.game->settings.mode != Mode::hide) return false;
    const auto *p = standing(*s.game, player);
    return p && !p->up && !p->out;
}

bool local_meat_game() noexcept {
    const auto &s = state();
    return s.game && s.game->settings.mode == Mode::meat && s.game->state && s.game->state->phase == Phase::playing &&
           standing(*s.game, self_id(s));
}
bool local_session() noexcept { return state().session; }

NativeMatch native_match() {
    auto &s = state();
    std::lock_guard lock(s.hud_mutex);
    return s.native;
}
std::uint64_t flag_game() noexcept { return state().flaggable.load(); }
bool countdown_active() noexcept { return state().counting_down.load(); }
std::string flag_placed(const std::array<float, 3> &spot, float yaw_degrees) {
    auto &s = state();
    if (!s.flaggable.load()) return "error: no game of yours to place a flag for.";
    std::lock_guard lock(s.hud_mutex);
    s.pending_flag = std::pair{spot, yaw_degrees};
    return "Flag placed: the game starts there.";
}
std::string apply_flag(State &s, const Vec3 &spot, float yaw_degrees) {
    if (!s.game || !s.game->leading) return "error: no game of yours to place a flag for.";
    auto &g = *s.game;
    if (g.referee) return "error: the game has started already.";
    g.settings.spawn = spot;
    g.settings.has_spawn = true;
    // Deathrace: the flag is the start gate (the route's checkpoints are placed from it on).
    if (g.settings.mode == Mode::race) {
        if (g.settings.points.empty()) {
            g.settings.points.push_back(spot);
            g.settings.yaws.assign(1, yaw_degrees);
            g.settings.widths.assign(1, std::clamp(g.settings.radius, min_gate_half_width, max_gate_half_width));
        } else {
            g.settings.points.front() = spot;
            if (g.settings.yaws.empty()) g.settings.yaws.push_back(yaw_degrees);
            else g.settings.yaws.front() = yaw_degrees;
        }
    }
    s.flag_key = game_key(g);
    s.flag_yaw = yaw_degrees;
    settings_changed(g);
    logging::log(logging::Level::info, logging::Channel::runtime, "Game modes: native flag for {} at ({:.1f}, {:.1f}, {:.1f}), facing {:.0f}.",
                 mode_name(g.settings.mode), spot[0], spot[1], spot[2], yaw_degrees);
    return std::format("{} flag placed. Everyone starts here.", mode_name(g.settings.mode));
}

overlay::ModesHud hud() {
    auto &s = state();
    overlay::ModesHud h;
    {
        std::lock_guard lock(s.hud_mutex);
        if (!s.hud.active && s.hud.offers.empty() && s.hud.invite.empty()) return {};
        h = s.hud;
    }
    if (const auto view = latest_game_view()) {
        h.camera = view->world;
        h.vertical_fov = view->vertical_fov;
    }
    return h;
}
} // namespace dingosdk::modes
