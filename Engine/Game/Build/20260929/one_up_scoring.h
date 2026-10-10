#pragma once
#include <cstdint>

namespace dingosdk::game::build::v20260929::one_up_scoring {
// Compiled expression hashes and header contracts exported from build 25414733.
// These are resource+0x10 hashes, not EBX TypeNameHash values.
inline constexpr std::uint32_t start_graph = 0xa4d1121c; // ScoringTelemetryManager_Class__OnScorableLineStarted
inline constexpr std::uint32_t end_graph = 0x69843dad; // ScoringTelemetryManager_Class__OnScorableLineEnded, Value at input page 1 +40
inline constexpr std::uint32_t stats_end_graph = 0xe2d57480; // StatsManager__OnScorableLineEnded, Value at input page 1 +16
inline constexpr std::uint32_t sequence_end_graph = 0xafef12e9; // ScoringTelemetryManager__OnScorableSequenceEnded, page 1 +1080
inline constexpr std::uint32_t sequence_end_type = 2977686039U, sequence_type = 1606121107U;
inline constexpr std::uint32_t sequence_data_field = 3982548569U, sequence_score_field = 2099127164U, success_field = 3611278405U;
inline constexpr std::uint32_t end_type = 0xd7f51dda, line_type = 0xf95bfe33;
inline constexpr std::uint32_t line_data_field = 0xcdfa75f5, reason_field = 0x9f0bcbf5, score_field = 0x40bff667;
// ScoringDataResource/ActiveScoringData and the field changed by the shipped
// DisableLineMultiplierRule.Activate/Deactivate graphs (d0619689/3224985a).
inline constexpr std::uint32_t active_scoring_type = 0xa8b7e62f;
inline constexpr std::uint32_t settings_field = 0xf4525023, disable_lines_field = 0x24b77123;
inline constexpr std::uint16_t active_scoring_size = 1280, settings_size = 40;
inline constexpr bool graph_contract(std::uint32_t hash, std::uint32_t frame, std::uint32_t constants, std::uint32_t words) {
    return (hash == start_graph && frame == 0x40 && constants == 0x48 && words == 0x7b) ||
        (hash == end_graph && frame == 0x70 && constants == 0x2d0 && words == 0x408) ||
        (hash == stats_end_graph && frame == 0x50 && constants == 0 && words == 0x21) ||
        (hash == sequence_end_graph && frame == 0x470 && constants == 0x908 && words == 0xffe);
}
} // namespace dingosdk::game::build::v20260929::one_up_scoring
