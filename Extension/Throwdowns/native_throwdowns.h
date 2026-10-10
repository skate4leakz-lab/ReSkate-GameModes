#pragma once
#include <cstdint>

namespace dingosdk::multiplayer {
// Offline stand-ins for the answers throwdowns normally get from the backend
// (throwdowns_v1 mode catalogue) and the online roster (host player lookup).
void initialize_native_throwdowns(std::uintptr_t base) noexcept;
// Game thread, every client tick: adds the throwdown text retail never shipped (the
// S.K.A.T.E. title showed as ID_ACTIVITY_SKATE_TITLE, Spot Battle's details description as
// ID_ACTIVITY_SPOTBATTLE_DESC) to the game's own string-override store, the one the
// backend's client_strings_v1 fills, once the string database is loaded. Rechecked every
// 2 s, since a language change or a backend refresh clears that store.
void apply_throwdown_strings(std::uintptr_t base) noexcept;
// True while a client ThrowdownRegistration graph is asking IsOnline (0) or
// IsSessionReady (1) in hosted offline play. Every other graph stays native.
bool native_throwdown_ready(unsigned predicate) noexcept;
// Call after an expression returns: delivers a parameter request that the
// absent throwdowns_v1 service left pending for that expression.
void complete_native_throwdown_parameters(std::uintptr_t vm) noexcept;
// Before either native interpreter executes a marker input action. Restrict
// only verified local-client action entries belonging to a running 1-Up.
bool consume_one_up_marker_expression(std::uintptr_t vm,std::uint32_t pc) noexcept;
// Native player id of the hosted offline server's only real player, or zero.
std::uint32_t local_native_player_id() noexcept;
// True from the moment the local player creates or joins a throwdown until it
// ends, is left or destroyed. Retail mirrors this onto UIPlayerInfo, where the
// throwdown Quit action reads it; offline the SDK owns that record.
bool local_throwdown_active() noexcept;
// True while that throwdown is the local player's own (it created it).
bool local_throwdown_host() noexcept;
// QueueFilled confirmed the server activity containing the local player. Only
// removal from that same activity may clear its offline ownership flags.
void native_throwdown_activity_started(std::uintptr_t activity, std::uint32_t player) noexcept;
void native_throwdown_participant_left(std::uintptr_t activity, std::uint32_t player) noexcept;
// Stock host's selected player limit, before QueueFilled creates the activity.
void native_throwdown_player_limit(std::uint32_t limit) noexcept;
// Exact running activity owned by this one-player host, or zero.
std::uintptr_t native_solo_throwdown_activity(std::uint32_t player) noexcept;
// Confirmed solo exit also retires the native Toolbox's IsPendingQueue model.
// Consume on the UI thread while holding its native model write lock.
bool native_solo_throwdown_menu_cleanup_pending() noexcept;
void native_solo_throwdown_menu_cleaned() noexcept;
// The level unloads all of its native queues and activities.
void native_throwdown_level_left() noexcept;
// Only the 1-Up flag adapter calls this, after its temporary native queue exits.
void release_one_up_native_placeholder() noexcept;
}
