#pragma once
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/race_objects.h"
#include <cstdint>
#include <limits>

namespace dingosdk::modes::race_world::detail {
template<class ReadPointer>
bool checkpoint_blueprint(std::uintptr_t asset, std::uintptr_t base, ReadPointer read) {
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (!asset || asset > maximum - 8 || !base ||
        base > maximum - addr::race_objects::object_blueprint_type) return false;
    auto type = read(asset + 8);
    for (unsigned depth = 0; type && depth < 16; ++depth) {
        if (type == base + addr::race_objects::object_blueprint_type) return true;
        if (type > maximum - 0x20) return false;
        type = read(type + 0x20);
    }
    return false;
}
template<class ReadPointer>
bool checkpoint_resident(std::uintptr_t cached, std::uintptr_t resolved,
    std::uintptr_t base, ReadPointer read) {
    // A world can stream out/reload a bundle without changing the client
    // context. Readable old allocation bytes alone do not prove residency.
    return cached && resolved == cached && checkpoint_blueprint(resolved,base,read);
}
}
