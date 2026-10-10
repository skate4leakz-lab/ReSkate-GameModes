#pragma once
#include "one_up_match.h"
#include <vector>

namespace dingosdk::multiplayer::one_up {
struct NativeLine {
    Token token;
    std::uint64_t line{};
    double score{};
    std::uint32_t elapsed{};
    bool ended{}, landed{}, scored_trick{};
};
// Existing expression hooks call this BEFORE the native graph consumes its input.
void observe_native_line(std::uintptr_t vm) noexcept;
void prepare_native_lines(std::uintptr_t base, std::uint64_t world, bool in_world) noexcept;
bool native_lines_available() noexcept;
// Client-thread only. Uses the game's DisableLineMultiplierRule setting;
// restores its previous value when 1-Up ends or the world changes.
void update_native_scoring_rule(std::uintptr_t base, std::uint64_t world, bool active) noexcept;
void arm_native_lines(Token, bool allow_start, Time turn_at) noexcept;
std::vector<NativeLine> take_native_lines();
} // namespace dingosdk::multiplayer::one_up
