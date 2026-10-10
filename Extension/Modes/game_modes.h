#pragma once
#include "Extension/UI/Overlay/overlay.h"
#include <array>
#include <cstdint>
#include <optional>
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
    // Where their skater is, from the session's own pose updates (none without a recent pose).
    // Skate Tag, Infection and Hide & Seek are played on these: game modes send no positions.
    std::optional<std::array<float, 3>> at;
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
// Infection: whether this other player is infected now, so their skater wears the reaper here
// (game thread, from the session's outfit pass).
bool infected_look(std::uint64_t player) noexcept;
// Hide & Seek: whether this other player is hiding now, so their nametag, dot and chat bubbles
// stay off (game thread).
bool hidden_player(std::uint64_t player) noexcept;
// `mode grid on|off`: whether Throwdowns lays the cards out in a grid (any thread).
bool throwdown_grid() noexcept;
// `mode <verb> [arguments]` from the console dispatcher (game thread).
std::string command(std::string_view verb, const std::vector<std::string> &arguments);
// The HUD's snapshot, with the latest camera (any thread).
overlay::ModesHud hud();
// The local player's game for skate.'s own HUD widgets (the native countdown, the score block with
// its clock, the results board: Extension/UI/NativeMenu/one_up_menu.cpp draws them for 1-Up and for
// these). Inactive without a game the local player is in or leads (any thread).
struct NativeMatch {
    bool active{};
    std::uint64_t key{};             // the game: a new key starts a new intro and results
    std::uint8_t phase{};            // 1 setup, 2 countdown, 3 playing, 4 results (Phase)
    std::uint32_t remaining_ms{};    // the countdown's, the clock's or the results' time left
    std::uint32_t clock_ms{};        // a timed game's whole length (0: no clock)
    std::uint32_t countdown_ms{};    // the countdown's whole length
    std::string title;               // "SPOT JAM"
    std::string tagline;             // a line on how it is played, under the intro's title
    std::string headline;            // over the score block: "SKATE TAG  /  ZEE IS IT"
    std::string best, mine;          // the leader's value (by the crown) and the local player's
    struct Row {
        std::uint64_t id{};
        std::string name, value;
        bool self{}, up{}, out{};
    };
    std::vector<Row> rows;           // ranked, the leader first
    std::string winner;              // results: who won ("" for none)
    bool leading{};                  // the local player leads it (can start or end it)
    std::size_t players{};
};
NativeMatch native_match();
// The local player's game is counting down to GO: they start on their board and their controls
// wait for GO, as at the start of skate.'s own Throwdowns (any thread).
bool countdown_active() noexcept;
// `mode nav <name>` (development): a native screen change the Throwdowns menu tick sends (taken once).
std::string take_native_navigation();
// Native flag placement for a game set up here (one_up_placement.cpp, from the mode's Throwdowns
// panel): the flag's spot and the way it faces. The game starts there, its area centred on it.
std::string flag_placed(const std::array<float, 3> &spot, float yaw_degrees);
// The game a native flag was placed for: its key while it is set up or on (0: none).
std::uint64_t flag_game() noexcept;
// What the ReSkate menu's Game Modes page shows (any thread).
overlay::ModesMenu menu_view();
} // namespace dingosdk::modes
