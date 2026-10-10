#pragma once
#include "one_up_wire.h"
#include "throwdown_relay.h"
#include <map>
#include <string_view>

namespace dingosdk::multiplayer::one_up {
struct Offer { PlayerId leader{}; std::uint64_t match{}; std::string name; bool started{}; };
struct View {
    State state;
    PlayerId local{};
    std::uint64_t world{};
    std::map<PlayerId, std::string> names;
    std::vector<Offer> offers;
    std::uint32_t remaining{};
    Time published{};
    bool scoring_ready{}, can_create{}, participant{}, solo_test{}, positioning{};
    std::string status;
    std::string name(PlayerId id) const;
};
View view();
struct SpawnRequest {
    std::array<float,3> position{};
    std::uint64_t world{}, local{};
    unsigned seconds=20, players=4;
    Facing facing=default_facing;
};
// The native flag picker supplies this position, not the skater's current one.
void create_at_spawn(const SpawnRequest& request);
void queue(std::string_view command);
void receive(PlayerId sender, std::span<const std::uint8_t> bytes);
std::vector<std::vector<std::uint8_t>> tick(std::uintptr_t base, const ThrowdownRelayInput&);
bool waits_offboard() noexcept;
bool countdown_locks_input() noexcept;
// Participating in a running match, including waiting/eliminated skaters.
// Match spawns still use the internal teleport API; session markers cannot.
bool restricts_session_markers() noexcept;
bool hides(PlayerId) noexcept;
} // namespace dingosdk::multiplayer::one_up
