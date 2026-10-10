#pragma once
#include <cstdint>
namespace dingosdk::multiplayer {
void tick_native_one_up_menu(std::uintptr_t base, bool loading) noexcept;
bool release_native_one_up_menu(std::uintptr_t base) noexcept;
bool native_one_up_setup_open() noexcept;
bool native_one_up_hud_ready() noexcept;
// The 1-UP card's tile model on the Throwdowns page (0 while none): the game modes grid shows it too.
std::uint64_t native_one_up_card() noexcept;
}
