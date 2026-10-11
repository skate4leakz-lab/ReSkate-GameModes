#pragma once
#include <cstdint>
// Deathrace's checkpoint gates as real objects in the world: skate.'s own tutorial checkpoint arch
// (two glowing rings and a lightning bolt, each side) at every gate but the start (which already has
// the native Throwdown flag) and the finish (the stock finish variant). Replaces the overlay's drawn
// neon gates for a mode on skate.'s elements only. Client thread.
namespace dingosdk::modes::race_world {
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
// Forwarded from the shared native entity destruction hook (install_entity_hooks).
void entity_destroyed(std::uintptr_t entity) noexcept;
} // namespace dingosdk::modes::race_world
