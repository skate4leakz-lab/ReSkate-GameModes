#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace dingosdk::multiplayer::menu_data { class Context; }
namespace dingosdk::multiplayer::one_up {
// Only owns a native Throwdown created by this request. Ordinary Spot Battle
// placement, navigation and network events never enter this adapter.
bool begin_flag_placement(unsigned seconds, unsigned players, bool show_native_setup=false);
// Reserve the native setup navigation for this card. The tick publishes it
// only after previous flag cleanup; a rejected click cannot open Spot Battle.
bool arm_native_setup(unsigned seconds, unsigned players);
bool native_setup_active() noexcept;
void set_native_setup_options(unsigned seconds,unsigned players) noexcept;
bool flag_placement_active() noexcept;
// Includes the placed registration while its native waiting HUD owns the flag.
bool flag_registration_owned() noexcept;
bool owns_flag_registration(std::uint32_t mmid) noexcept;
void flag_registration_left() noexcept;
void tick_flag_placement(const menu_data::Context& context);
void observe_flag_graph(std::uint32_t graph) noexcept;
void observe_flag_created() noexcept;
void observe_flag_position(std::uint32_t mmid, const std::array<float, 3>& spawn,
    const std::array<float,16>* transform=nullptr) noexcept;
void observe_flag_exit(bool cancelling) noexcept;
void observe_flag_destroyed(std::uint32_t mmid) noexcept;
void abandon_flag_placement() noexcept;
std::string flag_placement_status();
bool flag_lobby_requested() noexcept;
void flag_lobby_shown() noexcept;
}
