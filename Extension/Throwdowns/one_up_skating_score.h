#pragma once
#include <cstdint>

namespace dingosdk::multiplayer::one_up {
// Build 25414733: ScorableSequenceType and DingoTrickCategory's native enum
// reflection/getters. Riding includes Roll and trickless air; BonusAction,
// ChallengeAction, Flumping and Wipeout are not skating tricks.
constexpr bool skating_trick_category(std::uint32_t category) {
    return (category>=1 && category<=5) || (category>=13 && category<=19);
}
constexpr bool skating_sequence(std::uint32_t sequence_type,bool has_skating_trick) {
    return sequence_type==1 && has_skating_trick;
}
}
