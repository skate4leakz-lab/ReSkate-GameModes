#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dingosdk::multiplayer {
struct SteamSocialPlayer {
    std::uint64_t id{};
    std::string name;
    bool online{}, playing{};
    // The public server or lobby a friend is skating in (its browser row's id), when they
    // are in one: what their game tells Steam friends (steam_friend_join.h). 0: none, a
    // private session, or a build that does not say.
    std::uint64_t session{};
    bool operator==(const SteamSocialPlayer &) const = default;
};
struct SteamSocialSnapshot {
    SteamSocialPlayer local;
    std::vector<SteamSocialPlayer> friends;
    std::uint64_t revision{};
};
// Read the game's existing Steam session. Never initializes Steam, changes
// presence, or sends friend/invite requests. Poll only on the client thread.
// The local player has no `session` here: the session model knows theirs.
std::shared_ptr<const SteamSocialSnapshot> steam_social_snapshot();
// A Steam user's profile picture (their medium avatar, 64 x 64 RGBA), once Steam has it: the first
// asks fetch it, so ask again until it is there. False while it is not (offline, no Steam, still
// loading, no avatar). Any thread.
bool steam_avatar(std::uint64_t steam_id, std::vector<std::uint8_t> &rgba, std::uint32_t &width, std::uint32_t &height);
}
