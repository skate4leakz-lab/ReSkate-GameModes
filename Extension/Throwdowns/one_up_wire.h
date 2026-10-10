#pragma once
#include "one_up_match.h"
#include "one_up_spawn.h"
#include <array>
#include <map>
#include <span>

namespace dingosdk::multiplayer::one_up {
constexpr std::uint8_t wire_version = 4;
enum class Action : std::uint8_t { snapshot, join, ready, leave, trick_score };
struct Message {
    Action action{};
    std::uint64_t match{}, world{}, leader{}, sequence{}, line{};
    std::uint32_t turn{}, elapsed{}, remaining{};
    bool flag{};
    double score{};
    std::array<float, 3> spot{};
    Facing facing=default_facing;
    State state;
    bool operator==(const Message&) const = default;
};
bool valid_message(const Message&) noexcept;
std::vector<std::uint8_t> encode(const Message&);
std::optional<Message> decode(std::span<const std::uint8_t>) noexcept;
// One reliable ordered stream per sender and match. Repeated messages do nothing. A bounded
// reorder window preserves trick order without duplicating any native points.
class InputOrder {
    std::uint64_t next_ = 1;
    std::map<std::uint64_t, Message> held_;
public:
    explicit InputOrder(std::uint64_t first = 1) : next_(first) {}
    std::vector<Message> push(Message);
    void reset() { next_ = 1; held_.clear(); }
};
bool apply_snapshot(State& current, const Message&, PlayerId authenticated_sender, std::uint64_t world);
} // namespace dingosdk::multiplayer::one_up
