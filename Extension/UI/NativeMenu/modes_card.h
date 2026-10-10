#pragma once
#include <cstdint>

// ReSkate game modes in skate.'s own Throwdowns menu: a fourth card, GAME MODES, beside S.K.A.T.E.,
// Spot Battle and Skate Jam. It opens a native page listing every mode (set up, join, start, leave).
// Its models are private copies of the native ones; the three stock cards keep their own. Client
// thread, from the native menu tick.
namespace dingosdk::multiplayer {
void tick_native_modes_card(std::uintptr_t base, bool loading) noexcept;
// Before a level unloads: our models out of the native menu. False when that failed.
bool release_native_modes_card(std::uintptr_t base) noexcept;
} // namespace dingosdk::multiplayer
