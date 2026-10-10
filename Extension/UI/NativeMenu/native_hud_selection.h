#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace dingosdk::multiplayer::menu_data {
struct HudRootCandidate {
    std::uint64_t handle{};
    bool active{};
    unsigned gameplay_stacks{};
};
struct NotificationMountNode {
    std::uint64_t model{};
    std::uint32_t type{};
    std::uint64_t content{};
};
struct NotificationMount {
    std::uint64_t presentation{}, model{};
    bool operator==(const NotificationMount&) const = default;
};
// The secondary HUD stack holds FoundationsNotificationViewModel wrappers,
// not NotificationViewModel directly. Resolve only mounted, known wrappers;
// scanning all registered notifications also includes unrendered defaults.
template<class Read> std::optional<NotificationMount> mounted_notification(
        std::uint64_t presentation, Read&& read) {
    for(unsigned depth=0; presentation && depth<8; ++depth) {
        const auto node=read(presentation);
        if(!node.model)return std::nullopt;
        if(node.type==0x1053b9ec)return NotificationMount{presentation,node.model};
        if(node.type!=0xffb83e48 || node.content==presentation)return std::nullopt;
        presentation=node.content;
    }
    return std::nullopt;
}
// Authored defaults are registered as roots too. They have no game-owned
// compass/scoring/notification items and are not connected to a HUD widget.
inline std::uint64_t mounted_hud_root(std::span<const HudRootCandidate> roots,
                                     std::uint64_t preferred = 0) {
    std::uint64_t selected{};
    unsigned most{};
    for (const auto& root : roots) {
        if (!root.handle || !root.active || !root.gameplay_stacks) continue;
        if (root.handle == preferred) return root.handle;
        if (root.gameplay_stacks > most) {
            selected = root.handle;
            most = root.gameplay_stacks;
        }
    }
    return selected;
}
} // namespace dingosdk::multiplayer::menu_data
