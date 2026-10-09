#pragma once
#include "Extension/UI/Overlay/overlay.h"
#include <cstdint>
#include <string>
#include <vector>

// The Bone Cam: Skate 2's X-ray bail view. When the local skater wipes out (in a Hall of Meat
// game, or on every bail if the player turns it on) the screen goes X-ray for a few seconds:
// every bone is drawn between the skater's real joints, a joint that stops dead cracks or breaks
// the bone beside it, and the injuries are listed. It never changes game speed: the game's own
// time scale steps rather than slows, and only a console command may change it.
namespace dingosdk::modes {
// Game thread, every client tick after the trainer's (it reads the bail count the trainer watches).
void tick_bone_cam(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept;
// `mode bonecam [meat|on|off|test]` (game thread).
std::string bone_cam_command(const std::vector<std::string> &arguments);
std::string bone_cam_setting(); // "meat", "on" or "off"
bool bone_cam_ringing() noexcept; // a concussion's ringing is on (not muted)
// The local skater just bailed (game_modes.cpp finds bails; any thread).
void bone_cam_bail() noexcept;
// Whether the skater's body stopped dead (lost 7 m/s within a third of a second) since the last
// call: the ground, a pole, a wall. Only followed while the Bone Cam would show or a Hall of Meat
// game is on.
bool bone_cam_slam() noexcept;
// Whether the skater's head, chest, shoulders or hips struck an object at speed (a pole, a tree, a
// rail: nearly stopped while the body was still flying) since the last call. Followed as
// bone_cam_slam is.
bool bone_cam_struck() noexcept;
// The overlay's snapshot, with the latest camera (any thread).
overlay::BoneCam bone_cam();
// From game_modes.cpp (game thread): the local player is playing a Hall of Meat game; a session is on.
bool local_meat_game() noexcept;
bool local_session() noexcept;
} // namespace dingosdk::modes
