#include "one_up_wire.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace dingosdk::multiplayer::one_up {
namespace {
bool steam(PlayerId id) { return (id >> 56) == 1 && ((id >> 52) & 15) == 1 && (id & 0xffffffffULL); }
struct Writer {
    std::vector<std::uint8_t> bytes;
    void put(std::uint64_t v, unsigned n) { for (unsigned i = 0; i < n; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (i * 8))); }
    void real(double v) { put(std::bit_cast<std::uint64_t>(v), 8); }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    std::uint64_t get(unsigned n) {
        if (n > bytes.size() - at) throw std::invalid_argument("Truncated 1-Up message");
        std::uint64_t v{}; for (unsigned i = 0; i < n; ++i) v |= std::uint64_t{bytes[at++]} << (i * 8); return v;
    }
    bool flag() { const auto v = get(1); if (v > 1) throw std::invalid_argument("Invalid flag"); return v != 0; }
    double real() { return std::bit_cast<double>(get(8)); }
};
}
bool valid_message(const Message& m) noexcept {
    if (!m.match || !m.world || !steam(m.leader) || static_cast<unsigned>(m.action) > static_cast<unsigned>(Action::trick_score)) return false;
    if (m.action == Action::snapshot) {
        return m.state.match == m.match && m.state.world == m.world && m.state.leader == m.leader && valid_state(m.state) &&
            std::all_of(m.state.players.begin(), m.state.players.end(), [](const Player& p) { return steam(p.id); }) &&
            m.remaining <= 120000 && valid_facing(m.facing) && std::all_of(m.spot.begin(), m.spot.end(), [](float x) { return std::isfinite(x) && std::abs(x) < 1000000; });
    }
    if (!m.sequence || m.sequence == UINT64_MAX) return false;
    if (m.action == Action::trick_score)
        return m.turn && m.line && m.elapsed <= 125000 && std::isfinite(m.score) && m.score >= 0 && m.score <= 2147483647.0;
    return true;
}
std::vector<std::uint8_t> encode(const Message& m) {
    if (!valid_message(m)) throw std::invalid_argument("Invalid 1-Up message");
    Writer w; w.put(wire_version, 1); w.put(static_cast<unsigned>(m.action), 1);
    w.put(m.match, 8); w.put(m.world, 8); w.put(m.leader, 8);
    if (m.action == Action::snapshot) {
        const auto& s = m.state;
        w.put(s.revision, 8); w.put(s.active, 8); w.put(s.winner, 8); w.put(s.judged_player, 8);
        w.put(static_cast<unsigned>(s.phase), 1); w.put(s.turn, 4); w.real(s.target); w.real(s.best); w.real(s.judged_score); w.put(s.judged_success, 1);
        for (auto v : {s.config.turn_ms, s.config.grace_ms, s.config.countdown_ms, s.config.delivery_ms, s.config.feedback_ms, s.config.max_players}) w.put(v, 4);
        w.put(m.remaining, 4); for (auto v : m.spot) w.put(std::bit_cast<std::uint32_t>(v), 4);
        for(auto v:m.facing)w.put(std::bit_cast<std::uint32_t>(v),4);
        w.put(s.players.size(), 1);
        for (const auto& p : s.players) { w.put(p.id, 8); w.put(p.penalties, 1); w.put(p.ready, 1); w.put(p.connected, 1); }
        w.put(s.notice.size(), 1); for (unsigned char c : s.notice) w.put(c, 1);
    } else {
        w.put(m.sequence, 8); w.put(m.flag, 1);
        if (m.action == Action::trick_score) {
            w.put(m.turn, 4); w.put(m.line, 8); w.put(m.elapsed, 4); w.real(m.score);
        }
    }
    return std::move(w.bytes);
}
std::optional<Message> decode(std::span<const std::uint8_t> bytes) noexcept {
    try {
        if (bytes.size() > 1024) return {};
        Reader r{bytes}; if (r.get(1) != wire_version) return {};
        Message m; m.action = static_cast<Action>(r.get(1)); m.match = r.get(8); m.world = r.get(8); m.leader = r.get(8);
        if (m.action == Action::snapshot) {
            auto& s = m.state; s.match = m.match; s.world = m.world; s.leader = m.leader;
            s.revision = r.get(8); s.active = r.get(8); s.winner = r.get(8); s.judged_player = r.get(8);
            s.phase = static_cast<Phase>(r.get(1)); s.turn = static_cast<std::uint32_t>(r.get(4));
            s.target = r.real(); s.best = r.real(); s.judged_score = r.real(); s.judged_success = r.flag();
            for (auto* v : {&s.config.turn_ms, &s.config.grace_ms, &s.config.countdown_ms, &s.config.delivery_ms, &s.config.feedback_ms, &s.config.max_players}) *v = static_cast<std::uint32_t>(r.get(4));
            m.remaining = static_cast<std::uint32_t>(r.get(4));
            for (auto& v : m.spot) v = std::bit_cast<float>(static_cast<std::uint32_t>(r.get(4)));
            for(auto& v:m.facing)v=std::bit_cast<float>(static_cast<std::uint32_t>(r.get(4)));
            const auto count = r.get(1); if (count > 6) return {};
            for (std::uint64_t i = 0; i < count; ++i) {
                Player p; p.id = r.get(8); p.penalties = static_cast<std::uint8_t>(r.get(1)); p.ready = r.flag(); p.connected = r.flag(); s.players.push_back(p);
            }
            const auto count_text = r.get(1); if (count_text > 192) return {};
            for (std::uint64_t i = 0; i < count_text; ++i) { const auto c = r.get(1); if (c < 32 || c > 126) return {}; s.notice.push_back(static_cast<char>(c)); }
        } else {
            m.sequence = r.get(8); m.flag = r.flag();
            if (m.action == Action::trick_score) {
                m.turn = static_cast<std::uint32_t>(r.get(4)); m.line = r.get(8); m.elapsed = static_cast<std::uint32_t>(r.get(4)); m.score = r.real();
            }
        }
        if (r.at != bytes.size() || !valid_message(m)) return {};
        return m;
    } catch (...) { return {}; }
}
std::vector<Message> InputOrder::push(Message m) {
    std::vector<Message> ready;
    if (!m.sequence || m.sequence < next_ || m.sequence - next_ >= 64 || !valid_message(m) || m.action == Action::snapshot) return ready;
    held_.try_emplace(m.sequence, std::move(m));
    while (true) {
        auto it = held_.find(next_); if (it == held_.end()) break;
        ready.push_back(std::move(it->second)); held_.erase(it); ++next_;
    }
    return ready;
}
bool apply_snapshot(State& current, const Message& m, PlayerId sender, std::uint64_t world) {
    if (m.action != Action::snapshot || sender != m.leader || m.world != world || !valid_message(m) ||
        (current.match && (current.match != m.match || current.leader != m.leader || m.state.revision <= current.revision))) return false;
    current = m.state; return true;
}
} // namespace dingosdk::multiplayer::one_up
