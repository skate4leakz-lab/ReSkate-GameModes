#include "mode_rules.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dingosdk::modes {
namespace {
bool finite(const Vec3 &v) noexcept { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
float horizontal_distance(const Vec3 &a, const Vec3 &b) noexcept {
    const float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}
std::int32_t clamp_score(double value) noexcept {
    if (!std::isfinite(value) || value <= 0) return 0;
    return static_cast<std::int32_t>(std::min(value, 2.0e9));
}

struct Writer {
    std::vector<std::uint8_t> bytes;
    void integer(std::uint64_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void real(float value) { integer(std::bit_cast<std::uint32_t>(value), 4); }
    void vec(const Vec3 &v) {
        for (const auto c : v) real(c);
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    bool ok = true;
    std::uint64_t integer(unsigned size) {
        if (!ok || size > bytes.size() - at) { ok = false; return 0; }
        std::uint64_t value{};
        for (unsigned i = 0; i < size; ++i) value |= std::uint64_t{bytes[at + i]} << (8 * i);
        at += size;
        return value;
    }
    float real() { return std::bit_cast<float>(static_cast<std::uint32_t>(integer(4))); }
    Vec3 vec() { return {real(), real(), real()}; }
};

bool valid_settings(const Settings &s) noexcept {
    if (!std::any_of(std::begin(all_modes), std::end(all_modes), [&](Mode m) { return m == s.mode; })) return false;
    if (s.duration_s < 30 || s.duration_s > 3600 || s.turn_s < 10 || s.turn_s > 120 || s.strikes < 1 || s.strikes > 5)
        return false;
    if (!(s.radius >= 1.0f && s.radius <= 50.0f)) return false;
    if (s.corners.size() > max_corners || s.points.size() > max_points) return false;
    if (!s.yaws.empty() && s.yaws.size() != s.points.size()) return false;
    if (!s.widths.empty() && s.widths.size() != s.points.size()) return false;
    if (!std::all_of(s.widths.begin(), s.widths.end(), [](float w) { return w >= min_gate_half_width && w <= max_gate_half_width; })) return false;
    if (!std::all_of(s.yaws.begin(), s.yaws.end(), [](float y) { return std::isfinite(y); }) || (s.has_spawn && !finite(s.spawn))) return false;
    if (s.area_radius != 0.0f && !(s.area_radius >= min_area_radius && s.area_radius <= max_area_radius && s.corners.size() == 1))
        return false;
    return std::all_of(s.corners.begin(), s.corners.end(), finite) && std::all_of(s.points.begin(), s.points.end(), finite);
}
} // namespace

std::string_view mode_name(Mode m) noexcept {
    switch (m) {
    case Mode::jam: return "Spot Jam";
    case Mode::one_up: return "1-Up";
    case Mode::meat: return "Hall of Meat";
    case Mode::race: return "Deathrace";
    case Mode::domination: return "Domination";
    case Mode::graffiti: return "Graffiti";
    case Mode::tag: return "Skate Tag";
    }
    return "Game";
}
std::string_view mode_key(Mode m) noexcept {
    switch (m) {
    case Mode::jam: return "jam";
    case Mode::one_up: return "1up";
    case Mode::meat: return "meat";
    case Mode::race: return "race";
    case Mode::domination: return "domination";
    case Mode::graffiti: return "graffiti";
    case Mode::tag: return "tag";
    }
    return "";
}
std::string_view mode_summary(Mode m) noexcept {
    switch (m) {
    case Mode::jam: return "Land lines inside the area. Every line adds to your score; highest total wins.";
    case Mode::one_up: return "Take turns. Beat the last score or take a strike; last one standing wins.";
    case Mode::meat: return "Bail as hard as you can. Speed, drops and tumbling score; most meat wins.";
    case Mode::race: return "Hit every checkpoint in order. First to the last one wins.";
    case Mode::domination: return "Take spots with your best line there. Every second you hold one scores.";
    case Mode::graffiti: return "Grind it, manual it, gap it: what you skate takes your colour. A bigger line steals it. Most tags wins.";
    case Mode::tag: return "One player is it: get close to tag someone else. No tag-backs. Least time spent it wins.";
    }
    return "";
}
std::optional<Mode> parse_mode(std::string_view text) noexcept {
    std::string key;
    for (const char c : text) key += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (key == "jam" || key == "spotjam" || key == "spot_jam") return Mode::jam;
    if (key == "1up" || key == "1-up" || key == "oneup") return Mode::one_up;
    if (key == "meat" || key == "hallofmeat" || key == "hom") return Mode::meat;
    if (key == "race" || key == "deathrace") return Mode::race;
    if (key == "domination" || key == "dom") return Mode::domination;
    if (key == "graffiti" || key == "thps") return Mode::graffiti;
    if (key == "tag" || key == "skatetag" || key == "skate_tag") return Mode::tag;
    return std::nullopt;
}
bool timed(Mode m) noexcept { return m != Mode::one_up; }

std::string missing(const Settings &s) {
    switch (s.mode) {
    case Mode::race:
        if (s.points.size() < 2) return "Deathrace needs a route: a start and a finish (PLACE ROUTE).";
        break;
    case Mode::domination:
        if (s.points.empty()) return "Domination needs at least 1 spot: ride to each and use `mode point`.";
        break;

    default: break;
    }
    if (s.area_radius == 0.0f && (s.corners.size() == 1 || s.corners.size() == 2))
        return "An area needs at least 3 corners (or clear them to play without one).";
    return {};
}

float gate_half_width(const Settings &s, std::size_t gate) noexcept { return gate < s.widths.size() ? s.widths[gate] : s.radius; }
bool has_area(const Settings &s) noexcept { return s.area_radius > 0.0f ? !s.corners.empty() : s.corners.size() >= 3; }
bool inside(const Settings &s, const Vec3 &p) noexcept {
    if (s.area_radius > 0.0f && !s.corners.empty()) return horizontal_distance(p, s.corners[0]) <= s.area_radius;
    return inside(s.corners, p);
}

bool inside(const std::vector<Vec3> &corners, const Vec3 &p) noexcept {
    if (corners.size() < 3) return true;
    bool in = false;
    for (std::size_t i = 0, j = corners.size() - 1; i < corners.size(); j = i++) {
        const auto &a = corners[i], &b = corners[j];
        if ((a[2] > p[2]) != (b[2] > p[2]) && p[0] < (b[0] - a[0]) * (p[2] - a[2]) / (b[2] - a[2]) + a[0]) in = !in;
    }
    return in;
}
Vec3 area_centre(const std::vector<Vec3> &corners) noexcept {
    Vec3 c{};
    if (corners.empty()) return c;
    for (const auto &p : corners)
        for (int i = 0; i < 3; ++i) c[i] += p[i];
    for (auto &v : c) v /= static_cast<float>(corners.size());
    return c;
}

namespace {
// How far a point is from a path seen from above, and whether it is near the path's height.
float distance_to_path(const Vec3 &p, const std::vector<Vec3> &path) noexcept {
    float best = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const auto &a = path[i], &b = path[i + 1];
        const float dx = b[0] - a[0], dz = b[2] - a[2], length = dx * dx + dz * dz;
        const float t = length > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[2] - a[2]) * dz) / length, 0.0f, 1.0f) : 0.0f;
        const float x = a[0] + dx * t - p[0], y = a[1] + (b[1] - a[1]) * t - p[1], z = a[2] + dz * t - p[2];
        if (std::abs(y) <= 1.0f) best = std::min(best, std::sqrt(x * x + z * z));
    }
    if (path.size() == 1 && std::abs(path[0][1] - p[1]) <= 1.0f) best = horizontal_distance(p, path[0]);
    return best;
}
bool valid_tag(const Tag &t) noexcept {
    if (t.kind != TagKind::grind && t.kind != TagKind::gap) return false;
    if (t.path.size() < 2 || t.path.size() > max_tag_points || (t.kind == TagKind::gap && t.path.size() != 2)) return false;
    return std::all_of(t.path.begin(), t.path.end(), finite);
}
} // namespace

bool same_tag(const Tag &a, const Tag &b) noexcept {
    if (a.kind != b.kind || a.path.empty() || b.path.empty()) return false;
    if (a.kind == TagKind::gap)
        return horizontal_distance(a.path.front(), b.path.front()) <= 3.0f && horizontal_distance(a.path.back(), b.path.back()) <= 3.0f &&
               std::abs(a.path.front()[1] - b.path.front()[1]) <= 1.5f;
    // Grinds on the same curb or rail: most of the shorter one runs along the other.
    const auto &shorter = a.path.size() <= b.path.size() ? a : b, &longer = &shorter == &a ? b : a;
    std::size_t near = 0;
    for (const auto &p : shorter.path)
        if (distance_to_path(p, longer.path) <= 1.2f) ++near;
    return near * 2 >= shorter.path.size();
}
Vec3 tag_centre(const Tag &t) noexcept {
    if (t.kind == TagKind::gap && t.path.size() == 2) return t.path.back(); // a gap belongs where it lands
    return area_centre(t.path);
}
std::vector<Vec3> thin_path(const std::vector<Vec3> &path, std::size_t count) {
    if (path.size() <= count || count < 2) return path;
    std::vector<float> along(path.size());
    for (std::size_t i = 1; i < path.size(); ++i) {
        float d = 0;
        for (int k = 0; k < 3; ++k) d += (path[i][k] - path[i - 1][k]) * (path[i][k] - path[i - 1][k]);
        along[i] = along[i - 1] + std::sqrt(d);
    }
    std::vector<Vec3> result;
    for (std::size_t n = 0; n < count; ++n) {
        const float target = along.back() * static_cast<float>(n) / static_cast<float>(count - 1);
        std::size_t i = 1;
        while (i + 1 < path.size() && along[i] < target) ++i;
        const float span = along[i] - along[i - 1], t = span > 0 ? std::clamp((target - along[i - 1]) / span, 0.0f, 1.0f) : 0.0f;
        result.push_back({path[i - 1][0] + (path[i][0] - path[i - 1][0]) * t, path[i - 1][1] + (path[i][1] - path[i - 1][1]) * t,
                          path[i - 1][2] + (path[i][2] - path[i - 1][2]) * t});
    }
    return result;
}
std::optional<std::size_t> spot_at(const Settings &s, const Vec3 &p) noexcept {
    std::optional<std::size_t> best;
    float nearest = s.radius;
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        const float d = horizontal_distance(p, s.points[i]);
        if (d <= nearest && std::abs(p[1] - s.points[i][1]) <= 8.0f) { nearest = d; best = i; }
    }
    return best;
}

std::int32_t trick_score(const JumpSample &j) noexcept {
    // A hop or a step off a kerb is not a trick.
    if (!(j.air_time >= 0.2f)) return 0;
    double score = 100;
    score += j.air_time * 300.0;
    score += std::clamp(j.height, 0.0f, 30.0f) * 120.0;
    score += std::clamp(j.distance, 0.0f, 40.0f) * 10.0;
    const float spin = std::abs(j.spin);
    score += std::floor((spin + 30.0f) / 180.0f) * 250.0; // each 180 (a little short still counts)
    score += std::floor((j.flip + 60.0f) / 360.0f) * 600.0; // body flips and rolls
    if (j.board_turn >= 1800.0f) score += 350;            // the board spun hard: a flip trick
    else if (j.board_turn >= 900.0f) score += 200;
    return std::min(clamp_score(score), 20000);
}
std::int32_t line_score(std::int64_t sum, unsigned tricks) noexcept {
    if (sum <= 0 || !tricks) return 0;
    const double multiplier = std::min(6.0, 1.0 + 0.5 * (tricks - 1));
    return clamp_score(static_cast<double>(sum) * multiplier);
}
std::int32_t bail_score(const BailSample &b) noexcept {
    const double speed = std::clamp(b.speed, 0.0f, 60.0f), drop = std::clamp(b.drop, 0.0f, 60.0f),
                 tumble = std::clamp(b.tumble, 0.0f, 10.0f);
    return clamp_score(speed * speed * 4.0 + speed * 30.0 + drop * 250.0 + tumble * 150.0);
}

// ---- wire
namespace {
constexpr std::size_t max_tags_per_message = 12;
void write_tags(Writer &w, const std::vector<Tag> &tags, std::size_t limit) {
    if (tags.size() > limit) throw std::invalid_argument("too many game mode tags");
    w.integer(tags.size(), 1);
    for (const auto &t : tags) {
        if (!valid_tag(t)) throw std::invalid_argument("invalid game mode tag");
        w.integer(static_cast<std::uint8_t>(t.kind), 1);
        w.integer(t.path.size(), 1);
        for (const auto &p : t.path) w.vec(p);
    }
}
bool read_tags(Reader &r, std::vector<Tag> &tags, std::size_t limit) {
    const auto count = r.integer(1);
    if (!r.ok || count > limit) return false;
    for (std::uint64_t i = 0; i < count && r.ok; ++i) {
        Tag t;
        t.kind = static_cast<TagKind>(r.integer(1));
        const auto points = r.integer(1);
        if (!r.ok || points > max_tag_points) return false;
        for (std::uint64_t k = 0; k < points && r.ok; ++k) t.path.push_back(r.vec());
        if (!r.ok || !valid_tag(t)) return false;
        tags.push_back(std::move(t));
    }
    return r.ok;
}
} // namespace
std::vector<std::uint8_t> encode(const Message &m) {
    Writer w;
    w.integer(wire_magic, 1);
    w.integer(m.version, 1);
    w.integer(static_cast<std::uint8_t>(m.kind), 1);
    w.integer(m.leader, 8);
    w.integer(m.game, 4);
    switch (m.kind) {
    case Message::Kind::setup: {
        const auto &s = m.settings;
        if (!valid_settings(s)) throw std::invalid_argument("invalid game mode settings");
        w.integer(static_cast<std::uint8_t>(s.mode), 1);
        w.integer(s.duration_s, 4);
        w.integer(s.turn_s, 4);
        w.integer(s.strikes, 1);
        w.real(s.radius);
        w.real(s.area_radius);
        w.integer(s.yaws.size(), 1);
        for (const auto yaw : s.yaws) w.real(yaw);
        w.integer(s.widths.size(), 1);
        for (const auto width : s.widths) w.real(width);
        w.integer(s.has_spawn ? 1 : 0, 1);
        if (s.has_spawn) w.vec(s.spawn);
        w.integer(s.corners.size(), 1);
        for (const auto &p : s.corners) w.vec(p);
        w.integer(s.points.size(), 1);
        for (const auto &p : s.points) w.vec(p);
        break;
    }
    case Message::Kind::state:
        if (m.standings.size() > max_players || m.zones.size() > max_zones) throw std::invalid_argument("game mode state too large");
        w.integer(static_cast<std::uint8_t>(m.phase), 1);
        w.integer(m.remaining_ms, 4);
        w.integer(m.turn, 8);
        w.integer(static_cast<std::uint32_t>(m.target), 4);
        w.integer(m.standings.size(), 1);
        for (const auto &s : m.standings) {
            w.integer(s.player, 8);
            w.integer(static_cast<std::uint32_t>(s.score), 4);
            w.integer(static_cast<std::uint32_t>(s.aux), 4);
            w.integer((s.out ? 1u : 0u) | (s.up ? 2u : 0u), 1);
        }
        w.integer(m.zones.size(), 1);
        for (const auto &z : m.zones) {
            w.integer(z.zone, 1);
            w.integer(z.owner, 8);
            w.integer(static_cast<std::uint32_t>(z.best), 4);
        }
        if (m.calls.size() > max_calls) throw std::invalid_argument("too many game mode calls");
        w.integer(m.call_serial, 4);
        w.integer(m.calls.size(), 1);
        for (const auto &text : m.calls) {
            if (text.size() > max_call_length) throw std::invalid_argument("game mode call too long");
            w.integer(text.size(), 1);
            w.bytes.insert(w.bytes.end(), text.begin(), text.end());
        }
        break;
    case Message::Kind::event:
        if (!finite(m.at)) throw std::invalid_argument("invalid game mode event");
        w.integer(static_cast<std::uint8_t>(m.event), 1);
        w.integer(static_cast<std::uint32_t>(m.value), 4);
        w.integer(static_cast<std::uint32_t>(m.extra), 4);
        w.vec(m.at);
        w.integer(m.sequence, 4);
        write_tags(w, m.tags, max_line_tags);
        break;
    case Message::Kind::tags:
        if (m.first + m.tags.size() > max_tags) throw std::invalid_argument("game mode tags out of range");
        w.integer(m.first, 1);
        write_tags(w, m.tags, max_tags_per_message);
        break;
    case Message::Kind::leave:
    case Message::Kind::end:
    case Message::Kind::join: break;
    default: throw std::invalid_argument("unknown game mode message");
    }
    return w.bytes;
}

std::uint8_t message_version(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= 2 && bytes[0] == wire_magic ? bytes[1] : 0;
}

std::optional<Message> decode(std::span<const std::uint8_t> bytes) noexcept {
    try {
        Reader r{bytes};
        if (r.integer(1) != wire_magic) return std::nullopt;
        Message m;
        m.version = static_cast<std::uint8_t>(r.integer(1));
        if (m.version != wire_version) return std::nullopt;
        const auto kind = r.integer(1);
        if (kind < 1 || kind > 7) return std::nullopt;
        m.kind = static_cast<Message::Kind>(kind);
        m.leader = r.integer(8);
        m.game = static_cast<std::uint32_t>(r.integer(4));
        if (!m.leader || !m.game) return std::nullopt;
        switch (m.kind) {
        case Message::Kind::setup: {
            auto &s = m.settings;
            s.mode = static_cast<Mode>(r.integer(1));
            s.duration_s = static_cast<std::uint32_t>(r.integer(4));
            s.turn_s = static_cast<std::uint32_t>(r.integer(4));
            s.strikes = static_cast<std::uint8_t>(r.integer(1));
            s.radius = r.real();
            s.area_radius = r.real();
            const auto yaws = r.integer(1);
            if (yaws > max_points) return std::nullopt;
            for (std::uint64_t i = 0; i < yaws && r.ok; ++i) s.yaws.push_back(r.real());
            const auto widths = r.integer(1);
            if (widths > max_points) return std::nullopt;
            for (std::uint64_t i = 0; i < widths && r.ok; ++i) s.widths.push_back(r.real());
            s.has_spawn = r.integer(1) != 0;
            if (s.has_spawn) s.spawn = r.vec();
            const auto corners = r.integer(1);
            if (corners > max_corners) return std::nullopt;
            for (std::uint64_t i = 0; i < corners && r.ok; ++i) s.corners.push_back(r.vec());
            const auto points = r.integer(1);
            if (points > max_points) return std::nullopt;
            for (std::uint64_t i = 0; i < points && r.ok; ++i) s.points.push_back(r.vec());
            if (!r.ok || !valid_settings(s)) return std::nullopt;
            break;
        }
        case Message::Kind::state: {
            const auto phase = r.integer(1);
            if (phase < 1 || phase > 4) return std::nullopt;
            m.phase = static_cast<Phase>(phase);
            m.remaining_ms = static_cast<std::uint32_t>(r.integer(4));
            m.turn = r.integer(8);
            m.target = static_cast<std::int32_t>(r.integer(4));
            const auto standings = r.integer(1);
            if (standings > max_players) return std::nullopt;
            for (std::uint64_t i = 0; i < standings && r.ok; ++i) {
                Standing s;
                s.player = r.integer(8);
                s.score = static_cast<std::int32_t>(r.integer(4));
                s.aux = static_cast<std::int32_t>(r.integer(4));
                const auto flags = r.integer(1);
                s.out = flags & 1;
                s.up = flags & 2;
                m.standings.push_back(s);
            }
            const auto zones = r.integer(1);
            if (zones > max_zones) return std::nullopt;
            for (std::uint64_t i = 0; i < zones && r.ok; ++i) {
                ZoneOwner z;
                z.zone = static_cast<std::uint8_t>(r.integer(1));
                z.owner = r.integer(8);
                z.best = static_cast<std::int32_t>(r.integer(4));
                if (z.zone >= max_zones) return std::nullopt;
                m.zones.push_back(z);
            }
            m.call_serial = static_cast<std::uint32_t>(r.integer(4));
            const auto calls = r.integer(1);
            if (calls > max_calls) return std::nullopt;
            for (std::uint64_t i = 0; i < calls && r.ok; ++i) {
                const auto length = r.integer(1);
                if (!r.ok || length > max_call_length || length > bytes.size() - r.at) return std::nullopt;
                std::string text(reinterpret_cast<const char *>(bytes.data() + r.at), static_cast<std::size_t>(length));
                r.at += static_cast<std::size_t>(length);
                // Shown on screen: printable text only.
                for (auto &ch : text)
                    if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
                m.calls.push_back(std::move(text));
            }
            break;
        }
        case Message::Kind::event: {
            const auto event = r.integer(1);
            if (event < 1 || event > 4) return std::nullopt;
            m.event = static_cast<Event>(event);
            m.value = static_cast<std::int32_t>(r.integer(4));
            m.extra = static_cast<std::int32_t>(r.integer(4));
            m.at = r.vec();
            m.sequence = static_cast<std::uint32_t>(r.integer(4));
            if (!finite(m.at) || m.value < 0 || !read_tags(r, m.tags, max_line_tags)) return std::nullopt;
            break;
        }
        case Message::Kind::tags:
            m.first = static_cast<std::uint8_t>(r.integer(1));
            if (!read_tags(r, m.tags, max_tags_per_message) || m.first + m.tags.size() > max_tags) return std::nullopt;
            break;
        case Message::Kind::leave:
        case Message::Kind::end:
        case Message::Kind::join: break;
        default: return std::nullopt;
        }
        if (!r.ok || r.at != bytes.size()) return std::nullopt;
        return m;
    } catch (...) {
        return std::nullopt;
    }
}

// ---- referee
Referee::Referee(Settings settings, std::uint64_t leader, std::uint32_t game)
    : settings_(std::move(settings)), leader_(leader), game_(game) {
    // Graffiti's tags are found as they are skated; Domination's spots are set up front.
    if (settings_.mode == Mode::domination) owners_.resize(settings_.points.size());
    for (std::size_t i = 0; i < owners_.size(); ++i) owners_[i].zone = static_cast<std::uint8_t>(i);
}
Referee::Player *Referee::find(std::uint64_t id) noexcept {
    for (auto &p : players_)
        if (p.id == id) return &p;
    return nullptr;
}
const Referee::Player *Referee::find(std::uint64_t id) const noexcept {
    for (const auto &p : players_)
        if (p.id == id) return &p;
    return nullptr;
}
bool Referee::has_player(std::uint64_t player) const noexcept { return find(player) != nullptr; }
void Referee::set_name(std::uint64_t player, std::string name) {
    if (auto *p = find(player)) p->name = std::move(name);
}
std::string Referee::name_of(std::uint64_t id) const {
    const auto *p = find(id);
    return p && !p->name.empty() ? p->name : std::string("A player");
}
std::size_t Referee::still_in() const noexcept {
    return static_cast<std::size_t>(std::count_if(players_.begin(), players_.end(), [](const Player &p) { return !p.out; }));
}
void Referee::add_player(std::uint64_t player) {
    if (!player || find(player) || players_.size() >= max_players) return;
    if (phase_ != Phase::setup && phase_ != Phase::countdown) return;
    players_.push_back({player});
}
void Referee::remove_player(std::uint64_t player, std::uint64_t now_ms) {
    auto *p = find(player);
    if (!p || p->out) return;
    if (phase_ == Phase::setup || phase_ == Phase::countdown) {
        std::erase_if(players_, [&](const Player &q) { return q.id == player; });
        return;
    }
    p->out = true;
    call(name_of(player) + " left the game");
    if (phase_ != Phase::playing) return;
    // Skate Tag: the one who is it leaving hands it to the next player still in.
    if (settings_.mode == Mode::tag) {
        if (player == it_)
            for (const auto &q : players_)
                if (!q.out) {
                    make_it(q.id, 0, now_ms);
                    break;
                }
        if (players_.size() > 1 && still_in() <= 1) finish(now_ms);
        return;
    }
    if (settings_.mode == Mode::one_up && turn_ < players_.size() && players_[turn_].id == player) next_turn(now_ms);
    else if (settings_.mode == Mode::one_up && (still_in() == 0 || (players_.size() > 1 && still_in() <= 1))) finish(now_ms);
}
void Referee::start(std::uint64_t now_ms) {
    if (phase_ != Phase::setup) return;
    phase_ = Phase::countdown;
    phase_at_ = now_ms;
}
void Referee::begin_play(std::uint64_t now_ms) {
    phase_ = Phase::playing;
    phase_at_ = last_second_ = turn_at_ = now_ms;
    turn_ = 0;
    target_ = 0;
    call("GO!");
    if (settings_.mode == Mode::one_up && !players_.empty())
        call(name_of(players_[0].id) + " is up: set a score");
    // Skate Tag: someone starts it, picked by the game's id so every game starts differently.
    if (settings_.mode == Mode::tag && !players_.empty()) make_it(players_[game_ % players_.size()].id, 0, now_ms);
}
void Referee::make_it(std::uint64_t player, std::uint64_t by, std::uint64_t now_ms) {
    it_ = player;
    tagged_by_ = by;
    it_since_ = it_counted_ = now_ms;
    call(by ? std::format("{} tagged {}!", name_of(by), name_of(player)) : name_of(player) + " is it!");
}
// Skate Tag: whoever is it, within reach of another player who is not the one that just tagged
// them, tags them. Both positions must be fresh: an old one is somewhere they no longer are.
void Referee::try_tags(std::uint64_t now_ms) {
    const auto *it = find(it_);
    if (!it || it->out || !it->at_time || now_ms - it->at_time > position_fresh_ms) return;
    const float reach = settings_.radius;
    for (const auto &q : players_) {
        if (q.id == it_ || q.out || !q.at_time || now_ms - q.at_time > position_fresh_ms) continue;
        if (q.id == tagged_by_ && now_ms - it_since_ < no_tag_back_ms) continue; // no tag-backs
        const float dx = q.at[0] - it->at[0], dy = q.at[1] - it->at[1], dz = q.at[2] - it->at[2];
        if (dx * dx + dy * dy + dz * dz <= reach * reach) {
            make_it(q.id, it_, now_ms);
            return;
        }
    }
}
void Referee::finish(std::uint64_t now_ms) {
    if (phase_ == Phase::results) return;
    phase_ = Phase::results;
    phase_at_ = now_ms;
    const auto standings = state(now_ms).standings;
    if (!standings.empty()) call(name_of(standings.front().player) + " wins!");
}
void Referee::strike(Player &p, std::uint64_t now_ms, std::string_view why) {
    ++p.strikes;
    p.aux = p.strikes;
    call(std::format("{} {}: strike {} of {}", name_of(p.id), why, p.strikes, settings_.strikes));
    if (p.strikes >= settings_.strikes) {
        p.out = true;
        call(name_of(p.id) + " is out");
    }
    next_turn(now_ms);
}
void Referee::next_turn(std::uint64_t now_ms) {
    if (still_in() == 0 || (players_.size() > 1 && still_in() <= 1)) {
        finish(now_ms);
        return;
    }
    for (std::size_t step = 1; step <= players_.size(); ++step) {
        const auto index = (turn_ + step) % players_.size();
        if (!players_[index].out) {
            turn_ = index;
            break;
        }
    }
    turn_at_ = now_ms;
    call(target_ > 0 ? std::format("{} is up: beat {}", name_of(players_[turn_].id), target_)
                                 : name_of(players_[turn_].id) + " is up: set a score");
}
void Referee::event(std::uint64_t player, Event event, std::int32_t value, const Vec3 &at, std::uint32_t sequence,
                    std::uint64_t now_ms, const std::vector<Tag> &tags) {
    if (phase_ != Phase::playing) return;
    auto *p = find(player);
    if (!p || p->out || p->finished || sequence <= p->sequence) return;
    p->sequence = sequence;
    // Skate Tag runs on where everyone is (anywhere: leaving the area is dealt with by sending them back).
    if (settings_.mode == Mode::tag) {
        if (event != Event::position) return;
        for (const auto v : at)
            if (!std::isfinite(v) || std::abs(v) > 1e6f) return;
        p->at = at;
        p->at_time = now_ms;
        try_tags(now_ms);
        return;
    }
    if (event == Event::position) return;
    if (settings_.mode != Mode::race && !inside(settings_, at)) return; // out of the area: nothing counts
    switch (settings_.mode) {
    case Mode::tag: return; // handled above, from positions
    case Mode::jam:
        if (event != Event::line) return;
        p->score += value;
        p->aux = std::max(p->aux, value);
        break;
    case Mode::meat:
        if (event != Event::bail) return;
        p->score += value;
        if (value > p->aux) {
            p->aux = value;
            call(std::format("{} took {} of meat", name_of(player), value));
        }
        break;
    case Mode::one_up: {
        if (turn_ >= players_.size() || players_[turn_].id != player) return;
        if (event == Event::bail) {
            strike(*p, now_ms, "bailed");
        } else if (event == Event::line) {
            if (value > target_) {
                call(target_ > 0 ? std::format("{} beat it: {}", name_of(player), value)
                                             : std::format("{} set {}", name_of(player), value));
                target_ = value;
                p->score = std::max(p->score, value);
                next_turn(now_ms);
            } else {
                strike(*p, now_ms, std::format("fell short of {}", target_));
            }
        }
        break;
    }
    case Mode::race: {
        if (event != Event::checkpoint || value != p->score) return;
        if (settings_.points.empty() || static_cast<std::size_t>(value) >= settings_.points.size()) return;
        // Within the gate's own width (where the crossing was reported), with some slack for lag.
        if (horizontal_distance(at, settings_.points[static_cast<std::size_t>(value)]) >
            gate_half_width(settings_, static_cast<std::size_t>(value)) * 1.3f + 3.0f)
            return;
        ++p->score;
        if (static_cast<std::size_t>(p->score) == settings_.points.size()) {
            p->finished = true;
            p->aux = static_cast<std::int32_t>(std::min<std::uint64_t>(now_ms - phase_at_, 0x7fffffff));
            const auto place = std::count_if(players_.begin(), players_.end(), [](const Player &q) { return q.finished; });
            call(std::format("{} finished #{} in {:.1f} s", name_of(player), place, p->aux / 1000.0));
        }
        break;
    }
    case Mode::domination: {
        if (event != Event::line) return;
        const auto spot = spot_at(settings_, at);
        if (!spot || value <= owners_[*spot].best) return;
        if (owners_[*spot].owner != player) call(std::format("{} took spot {}", name_of(player), *spot + 1));
        owners_[*spot].owner = player;
        owners_[*spot].best = value;
        p->aux = std::max(p->aux, value);
        break;
    }
    case Mode::graffiti: {
        if (event != Event::line) return;
        // Every grind and gap in the line is tagged with it: a new tag, or a steal when the line
        // beats the one that holds it.
        std::size_t tagged = 0, stolen = 0;
        std::uint64_t from{};
        for (std::size_t t = 0; t < tags.size() && t < max_line_tags; ++t) {
            const auto &tag = tags[t];
            if (!valid_tag(tag) || !inside(settings_, tag_centre(tag))) continue;
            auto found = std::find_if(tags_.begin(), tags_.end(), [&](const Tag &known) { return same_tag(known, tag); });
            if (found == tags_.end()) {
                if (tags_.size() >= max_tags) continue;
                tags_.push_back(tag);
                owners_.push_back({static_cast<std::uint8_t>(tags_.size() - 1), 0, 0});
                found = tags_.end() - 1;
            }
            auto &owner = owners_[static_cast<std::size_t>(found - tags_.begin())];
            if (owner.owner == player) {
                owner.best = std::max(owner.best, value);
                continue;
            }
            if (value <= owner.best) continue;
            if (owner.owner) {
                ++stolen;
                from = owner.owner;
            } else {
                ++tagged;
            }
            owner.owner = player;
            owner.best = value;
        }
        if (stolen) call(stolen == 1 ? std::format("{} stole a tag from {}", name_of(player), name_of(from))
                                     : std::format("{} stole {} tags", name_of(player), stolen));
        else if (tagged) call(std::format("{} tagged {} spot{}", name_of(player), tagged, tagged == 1 ? "" : "s"));
        for (auto &q : players_) {
            q.score = q.aux = 0;
            for (const auto &o : owners_)
                if (o.owner == q.id) { ++q.score; q.aux += o.best; }
        }
        break;
    }
    }
}
bool Referee::tick(std::uint64_t now_ms) {
    const auto elapsed = now_ms >= phase_at_ ? now_ms - phase_at_ : 0;
    switch (phase_) {
    case Phase::countdown:
        if (elapsed >= countdown_ms) { begin_play(now_ms); return true; }
        return false;
    case Phase::playing:
        if (timed(settings_.mode) && elapsed >= settings_.duration_s * 1000ull) { finish(now_ms); return true; }
        if (settings_.mode == Mode::race && !players_.empty() &&
            std::all_of(players_.begin(), players_.end(), [](const Player &p) { return p.finished || p.out; })) {
            finish(now_ms);
            return true;
        }
        if (settings_.mode == Mode::domination) {
            while (now_ms >= last_second_ + 1000) {
                last_second_ += 1000;
                for (const auto &o : owners_)
                    if (auto *p = o.owner ? find(o.owner) : nullptr) ++p->score;
            }
        }
        if (settings_.mode == Mode::tag) {
            // The time spent it, in tenths of a second (the score: fewer wins).
            if (auto *p = find(it_)) {
                p->it_ms += now_ms - it_counted_;
                p->score = static_cast<std::int32_t>(std::min<std::uint64_t>(p->it_ms / 100, 0x7fffffff));
            }
            it_counted_ = now_ms;
        }
        if (settings_.mode == Mode::one_up && turn_ < players_.size() && now_ms >= turn_at_ + settings_.turn_s * 1000ull) {
            strike(players_[turn_], now_ms, "ran out of time");
            return true;
        }
        return false;
    default: return false;
    }
}
bool Referee::finished(std::uint64_t now_ms) const noexcept {
    return phase_ == Phase::results && now_ms >= phase_at_ + results_ms;
}
Message Referee::state(std::uint64_t now_ms) const {
    Message m;
    m.kind = Message::Kind::state;
    m.leader = leader_;
    m.game = game_;
    m.phase = phase_;
    const auto elapsed = now_ms >= phase_at_ ? now_ms - phase_at_ : 0;
    const auto left = [&](std::uint64_t total) { return static_cast<std::uint32_t>(total > elapsed ? total - elapsed : 0); };
    switch (phase_) {
    case Phase::countdown: m.remaining_ms = left(countdown_ms); break;
    case Phase::playing:
        if (settings_.mode == Mode::one_up) {
            const auto turn_elapsed = now_ms >= turn_at_ ? now_ms - turn_at_ : 0;
            const auto total = settings_.turn_s * 1000ull;
            m.remaining_ms = static_cast<std::uint32_t>(total > turn_elapsed ? total - turn_elapsed : 0);
        } else {
            m.remaining_ms = left(settings_.duration_s * 1000ull);
        }
        break;
    case Phase::results: m.remaining_ms = left(results_ms); break;
    default: break;
    }
    if (settings_.mode == Mode::one_up && phase_ == Phase::playing && turn_ < players_.size()) m.turn = players_[turn_].id;
    if (settings_.mode == Mode::tag && (phase_ == Phase::playing || phase_ == Phase::results)) m.turn = it_;
    m.target = target_;
    for (std::size_t i = 0; i < players_.size(); ++i) {
        const auto &p = players_[i];
        m.standings.push_back({p.id, p.score, p.aux, p.out, m.turn == p.id});
    }
    const auto mode = settings_.mode;
    std::stable_sort(m.standings.begin(), m.standings.end(), [mode, this](const Standing &a, const Standing &b) {
        if (mode == Mode::race) {
            const auto *pa = find(a.player), *pb = find(b.player);
            const bool fa = pa && pa->finished, fb = pb && pb->finished;
            if (fa != fb) return fa;
            if (fa) return a.aux < b.aux;
            return a.score > b.score;
        }
        if (mode == Mode::tag) { // least time spent it first
            if (a.out != b.out) return !a.out;
            return a.score < b.score;
        }
        if (mode == Mode::one_up) {
            if (a.out != b.out) return !a.out;
            if (a.aux != b.aux) return a.aux < b.aux; // fewer strikes
            return a.score > b.score;
        }
        if (a.score != b.score) return a.score > b.score;
        return a.aux > b.aux;
    });
    for (const auto &o : owners_)
        if (o.owner) m.zones.push_back(o);
    m.calls = recent_;
    m.call_serial = call_serial_;
    return m;
}
std::vector<std::string> Referee::take_calls() { return std::exchange(calls_, {}); }
void Referee::call(std::string text) {
    if (text.size() > max_call_length) text.resize(max_call_length);
    calls_.push_back(text);
    recent_.push_back(std::move(text));
    if (recent_.size() > max_calls) recent_.erase(recent_.begin());
    ++call_serial_;
}
} // namespace dingosdk::modes
