#pragma once
#include <cstdint>
namespace dingosdk::multiplayer {
void tick_native_one_up_menu(std::uintptr_t base, bool loading) noexcept;
bool release_native_one_up_menu(std::uintptr_t base) noexcept;
bool native_one_up_setup_open() noexcept;
bool native_one_up_hud_ready() noexcept;
}
