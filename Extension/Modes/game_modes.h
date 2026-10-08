#pragma once
#include "Extension/UI/Overlay/overlay.h"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ReSkate's game modes in the running game (rules: mode_rules.h). The game thread drives
// everything: the session tick hands over who is here and what arrived, and the console's
// `mode` command sets a game up. The local player's lines and bails come from what the
// Trainer measures each tick; the HUD reads a snapshot from any thread.
namespace dingosdk::modes {
struct SessionPlayer {
    std::uint64_t id{}; // Steam ID
    std::string name;
};
struct SessionInput {
    std::uint64_t local{};    // this player's Steam ID; 0 without a session (then playing alone)
    std::string local_name;
    std::vector<SessionPlayer> peers; // the other players in this world (never a dedicated server)
    bool in_world{};          // the world is loaded and playing
    std::uint64_t world{};    // changes whenever the map or world changes
    bool barred{};            // the local player's mods change scoring: they cannot play
};
// Session tick (game thread): runs the local game and returns the messages to send to
// every other player (dropped without a session).
std::vector<std::vector<std::uint8_t>> tick(const SessionInput &input);
// One game mode message from another player (game thread). False when the bytes are not one.
bool receive(std::uint64_t sender, std::span<const std::uint8_t> message);
// Lines for the local chat since the last call (game thread).
std::vector<std::string> take_notices();
// `mode <verb> [arguments]` from the console dispatcher (game thread).
std::string command(std::string_view verb, const std::vector<std::string> &arguments);
// The HUD's snapshot, with the latest camera (any thread).
overlay::ModesHud hud();
// What the ReSkate menu's Game Modes page shows (any thread).
overlay::ModesMenu menu_view();
} // namespace dingosdk::modes
