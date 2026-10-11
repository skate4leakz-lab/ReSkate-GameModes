#pragma once
#include <set>
#include <span>
#include <utility>

namespace dingosdk::modes::race_world::detail {
// Each native tutorial checkpoint owns four mesh entities. A Destroy callback
// can run inside creation, so incomplete groups must also drive reconciliation.
inline std::set<unsigned> pending_checkpoint_groups(const std::set<unsigned>& dirty,
                                                    std::span<const unsigned> live_parts) {
    auto pending=dirty;
    for(unsigned i=0;i<live_parts.size();++i)
        if(live_parts[i]!=4)pending.insert(i);
    return pending;
}
inline std::set<unsigned> take_checkpoint_groups(std::set<unsigned>& dirty,
                                                std::span<const unsigned> live_parts) {
    // Consume only the marks that precede this rebuild. Callbacks raised by
    // the rebuild remain in dirty for the next tick, even for the same group.
    std::set<unsigned> taken;
    taken.swap(dirty);
    return pending_checkpoint_groups(taken,live_parts);
}
}
