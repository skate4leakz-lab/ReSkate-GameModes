#pragma once
#include <cstdint>

// ReSkate game modes in skate.'s own Throwdowns menu: a card per mode after S.K.A.T.E., Spot Battle
// and Skate Jam, in a row that scrolls left and right (or, with `mode grid on`, a grid that scrolls
// up and down). A card sets its mode up and opens a native page to start, join, end or leave it.
// Its models are private copies of the native ones; the three stock cards keep their own. Client
// thread, from the native menu tick.
namespace dingosdk::multiplayer {
void tick_native_modes_card(std::uintptr_t base, bool loading) noexcept;
// Before a level unloads: our models out of the native menu. False when that failed.
bool release_native_modes_card(std::uintptr_t base) noexcept;
} // namespace dingosdk::multiplayer
