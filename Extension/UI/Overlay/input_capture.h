#pragma once
#include "Engine/Game/Input/playstation_report.h"
#include <atomic>
#include <cstdint>
#include <Windows.h>
#include <Xinput.h>
namespace dingosdk::overlay::detail {
extern thread_local unsigned overlay_input_access;
struct OverlayInputAccess {
    OverlayInputAccess() { ++overlay_input_access; }
    ~OverlayInputAccess() { --overlay_input_access; }
};
// The WM_INPUT packet the window procedure has already noted (note_raw_mouse_packet)
// while that message is handled on this thread; the game reading the same packet
// through GetRawInputData is then not noted a second time.
extern thread_local HRAWINPUT noted_raw_input;
bool block_polled_input();
extern std::atomic<std::uint16_t> hidden_game_buttons;
// All the player's input kept from the game (game modes' free-camera placing): block_polled_input
// is true while it is, as with the menu open. Held until this GetTickCount64 time: game modes
// publish it every tick, so a game modes tick that stops running cannot leave the game deaf.
extern std::atomic<std::uint64_t> game_input_paused_until;
inline bool game_input_paused() noexcept { return GetTickCount64() < game_input_paused_until.load(std::memory_order_relaxed); }
// Mouse wheel movement (WHEEL_DELTA units) the game did not get while its input was paused.
extern std::atomic<int> paused_wheel;
bool install_input_capture();
// Releases the D-pad in one PlayStation input report while hide_game_buttons keeps it
// (playstation_filter.cpp).
void release_playstation_dpad(PlayStationPad kind, std::uint8_t *report, unsigned long size) noexcept;
// Skate's controller mode registers the mouse with RIDEV_NOLEGACY, which stops
// the Windows cursor from moving. While the overlay owns the pointer the mouse
// is registered without it; the game's own registration returns on close.
// Call on the game window's thread; does nothing when nothing changed.
void sync_raw_mouse_registration();
// The same, when the caller already knows whether the overlay owns the pointer
// (owns_menu_cursor), as the window procedure does once per mouse message.
void sync_raw_mouse_registration(bool overlay_owns_pointer);
// One raw mouse packet, from whichever path the game read it (WM_INPUT,
// GetRawInputData or GetRawInputBuffer).
void note_raw_mouse_packet(const RAWMOUSE& mouse);
void note_raw_mouse_packet(const RAWMOUSE& mouse, bool overlay_owns_pointer);
// The game window's input messages (keys, mouse), for the input-mode log (input_capture.cpp).
void note_input_message(UINT message, WPARAM wp) noexcept;
}
