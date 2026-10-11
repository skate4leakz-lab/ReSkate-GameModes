#include "race_world.h"
#include "race_asset_contract.h"
#include "race_source_contract.h"
#include "race_group_reconciliation.h"
#include "game_modes.h"
#include "mode_rules.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/ai_skaters.h"
#include "Engine/Game/Build/20260929/skater_entities.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <stdexcept>
#include <set>
#include <vector>

// The real tutorial checkpoint arch (the game's own, confirmed by a friend's independent research
// against this exact build): spawned at each Deathrace gate but the start (the native flag already
// marks it) in place of the overlay's drawn neon gates, so the route is skate.'s own geometry, not
// ReSkate's. Each gate is four mesh entities (two rings, two bolts); a dedicated finish variant.
namespace dingosdk::modes::race_world {
namespace {
namespace e = addr::skater_entities;
namespace b = addr::ai_skaters;
using Matrix = std::array<float, 16>;
template <class T> T get(std::uintptr_t p) {
    T v{};
    memory::peek(p, v);
    return v;
}
std::uintptr_t ptr(std::uintptr_t p, std::uintptr_t offset = 0) { return get<std::uintptr_t>(p + offset); }
void require(bool ok, const char *why) {
    if (!ok) throw std::runtime_error(why);
}
template <std::size_t N> void prefix(std::uintptr_t base, std::uintptr_t rva, const std::array<unsigned char, N> &expected) {
    std::array<unsigned char, N> actual{};
    require(memory::peek_bytes(base + rva, actual.data(), N) && actual == expected, "Deathrace entity ABI differs.");
}
struct Gate {
    Vec3 at{};
    float yaw{}, width{};
    bool finish{};
    bool operator==(const Gate &) const = default;
};
struct Entity {
    std::uintptr_t entity{}, owner{}, context{};
    unsigned group{}, part{};
};
struct State {
    std::mutex mutex;
    std::vector<Entity> entities;
    std::vector<Gate> gates;
    std::array<std::uintptr_t, 6> assets{};
    std::array<std::uint16_t, 6> asset_domains{};
    std::uintptr_t context{};
    std::uint64_t world{}, retry{}, preview_at{};
    std::uint64_t game{};
    bool verified{};
    std::string error, waiting;
    std::set<unsigned> dirty;
};
State &state() {
    static auto *s = new State;
    return *s;
}
void waiting(State &s, std::string issue) {
    if (s.waiting == issue) return;
    s.waiting = std::move(issue);
    logging::log(logging::Level::debug, logging::Channel::runtime, "Deathrace world objects: {}", s.waiting);
}
// skate.'s own tutorial checkpoint arch: two glowing rings and a lightning bolt either side; a
// dedicated finish variant of each. Entries 0-2 the normal gate, 3-5 the finish.
constexpr std::array<const char *, 6> checkpoint_names{
    "World/ReSkate/Deathrace/SM_FX_GP_Checkpoint_A_AT_Gate", "World/ReSkate/Deathrace/SM_FX_GP_Checkpoint_A_AL_Gate",
    "World/ReSkate/Deathrace/SM_FX_GP_LightningBolt_A_Gate", "World/ReSkate/Deathrace/SM_FX_GP_Checkpoint_A_AT_Finish",
    "World/ReSkate/Deathrace/SM_FX_GP_Checkpoint_A_AL_Finish", "World/ReSkate/Deathrace/SM_FX_GP_LightningBolt_A_Finish"};
// A full domain lookup is bounded and done only when entering a race or loading its bundle. Six
// known-domain lookups validate the cached pointers thereafter. UI assets are in a child domain, so
// searching the skater's parents alone cannot find a private checkpoint loaded by the picker.
bool load_assets(std::uintptr_t base, State &s) {
    const auto find = game::native_data().find_asset;
    if (!find) return false;
    for (std::uint16_t domain = 0; domain < 0xbbf; ++domain) {
        if (!ptr(base + addr::engine::domain_owners, std::uintptr_t(domain) * 8)) continue;
        for (unsigned i = 0; i < checkpoint_names.size(); ++i)
            if (!s.assets[i]) {
                const auto asset = find(domain, checkpoint_names[i]);
                if (detail::checkpoint_blueprint(asset, base, [](auto at) { return ptr(at); })) {
                    s.assets[i] = asset;
                    s.asset_domains[i] = domain;
                }
            }
        if (std::all_of(s.assets.begin(), s.assets.end(), [](auto a) { return a != 0; })) return true;
    }
    return false;
}
bool assets_resident(std::uintptr_t base, const State &s) {
    const auto find = game::native_data().find_asset;
    if (!find) return false;
    for (unsigned i = 0; i < s.assets.size(); ++i) {
        if (!s.assets[i]) continue;
        if (!detail::checkpoint_resident(s.assets[i], find(s.asset_domains[i], checkpoint_names[i]), base, [](auto at) { return ptr(at); }))
            return false;
    }
    return true;
}
std::uintptr_t parent_bus(std::uintptr_t base, std::uintptr_t context) {
    return detail::checkpoint_parent_bus(
        base, context, [](auto at) { return ptr(at); }, [](auto at) { return get<std::uint32_t>(at); },
        [](auto at) { return get<std::uint8_t>(at); }, [](auto at) -> std::string {
            std::array<char, 256> text{};
            if (!at || !memory::peek_bytes(at, text.data(), text.size()) || std::find(text.begin(), text.end(), '\0') == text.end()) return {};
            return text.data();
        });
}
Entity create(std::uintptr_t base, std::uintptr_t parent, std::uintptr_t asset, const Matrix &matrix, unsigned group, unsigned part) {
    using CreateBus = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint8_t, std::uintptr_t);
    const auto bus = reinterpret_cast<CreateBus>(base + b::create_bus)(ptr(ptr(parent, 8), 0x48), parent, 0, 0, 0, 0);
    require(bus != 0, "Deathrace entity bus unavailable.");
    reinterpret_cast<int (*)(std::uintptr_t)>(base + b::acquire_bus)(bus);
    struct BusScope {
        std::uintptr_t base, bus;
        ~BusScope() { reinterpret_cast<void (*)(std::uintptr_t)>(base + e::release_reference)(bus); }
    } bus_scope{base, bus};
    alignas(16) std::array<unsigned char, 0x190> descriptor{};
    reinterpret_cast<void *(*)(void *, std::uintptr_t, std::uintptr_t, const void *)>(base + e::descriptor_init)(descriptor.data(), 0, bus,
                                                                                                                   matrix.data());
    struct Scope {
        std::uintptr_t base;
        void *data;
        ~Scope() { reinterpret_cast<void (*)(void *)>(base + e::descriptor_destroy)(static_cast<unsigned char *>(data) + 0x10); }
    } scope{base, descriptor.data()};
    const std::uint32_t id = 255;
    std::memcpy(descriptor.data() + 0x38, &id, 4);
    descriptor[0x151] = 0;
    std::array<std::uintptr_t, 3> result{};
    reinterpret_cast<void *(*)(void *, void *, std::uintptr_t, std::uintptr_t, std::uintptr_t)>(base + e::create_entity)(
        result.data(), descriptor.data(), asset, 0, 0);
    if (result[1]) reinterpret_cast<void (*)(std::uintptr_t)>(base + e::release_reference)(result[1]);
    require(result[0] != 0, "Native checkpoint mesh did not create an entity.");
    const Entity entity{result[0], ptr(result[0], 0x40), ptr(result[0], 0x20), group, part};
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        s.entities.push_back(entity);
    }
    reinterpret_cast<void (*)(std::uintptr_t, const void *, std::uintptr_t, std::uint8_t)>(base + e::initialize_placement)(entity.entity,
                                                                                                                             matrix.data(), 0, 1);
    return entity;
}
// Exact child transforms from the game's own checkpoint prefab. One size control scales the whole
// gate, including bolt placement, about ground level.
Matrix mesh_pose(const Gate &gate, unsigned part) {
    constexpr std::array<float, 4> sx{7.f, 7.f, -2.12332f, 2.13802f}, sy{7.f, 7.f, 2.12332f, 2.13802f};
    constexpr std::array<float, 4> right_y{0.f, 0.f, -.286054f, .0179417f}, up_x{0.f, 0.f, -.286054f, -.0179417f};
    constexpr std::array<float, 4> x{0.f, 0.f, -1.67701f, 1.70283f}, y{1.70747f, 1.70747f, 4.31834f, 4.34544f};
    constexpr std::array<float, 4> z{.00849477f, 0.f, .1f, .1f};
    const float yaw = gate.yaw * 3.14159265f / 180.f, co = std::cos(yaw), si = std::sin(yaw), width = gate.width * 2.f / 7.f;
    const float scale = gate.width / 6.f;
    const float a = sx[part] * width, bb = right_y[part] * scale, c = up_x[part] * width, d = sy[part] * scale,
                depth = (part < 2 ? 7.f : part == 2 ? 2.1425f : 2.1381f) * scale;
    return {co * a, bb, -si * a, 0, co * c, d, -si * c, 0, si * depth, 0, co * depth, 0, gate.at[0] + co * x[part] * width + si * z[part] * scale,
            gate.at[1] + y[part] * scale, gate.at[2] - si * x[part] * width + co * z[part] * scale, 1};
}
void clear(std::uintptr_t base) {
    auto &s = state();
    std::vector<Entity> old;
    {
        std::lock_guard lock(s.mutex);
        old.swap(s.entities);
        s.gates.clear();
        s.dirty.clear();
    }
    // No lock across native destruction: the shared hook calls us back.
    for (const auto &entity : old)
        if (ptr(entity.entity, 0x20) == entity.context && ptr(entity.entity, 0x40) == entity.owner)
            reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(base + e::destroy_entity)(entity.entity, entity.owner);
}
void clear_group(std::uintptr_t base, unsigned group) {
    auto &s = state();
    std::vector<Entity> old;
    {
        std::lock_guard lock(s.mutex);
        for (const auto &entity : s.entities)
            if (entity.group == group) old.push_back(entity);
        std::erase_if(s.entities, [&](const Entity &entity) { return entity.group == group; });
    }
    for (const auto &entity : old)
        if (ptr(entity.entity, 0x20) == entity.context && ptr(entity.entity, 0x40) == entity.owner)
            reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(base + e::destroy_entity)(entity.entity, entity.owner);
}
} // namespace

void entity_destroyed(std::uintptr_t entity) noexcept {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    for (const auto &e0 : s.entities)
        if (e0.entity == entity) s.dirty.insert(e0.group);
    std::erase_if(s.entities, [&](const Entity &e0) { return e0.entity == entity; });
}

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto &s = state();
    try {
        const auto context = ptr(client, 8);
        const auto h = modes::hud();
        const auto match = modes::native_match();
        const auto world = multiplayer::native_throwdown_world();
        const bool active = h.active && h.route && match.phase != 4; // 4: Phase::results
        if (s.context && s.context != context) {
            std::lock_guard lock(s.mutex);
            s.entities.clear();
            s.gates.clear();
            s.dirty.clear();
            s.assets = {};
            s.context = 0;
        }
        if (!ready || !context) return;
        const auto tls_array = static_cast<std::uintptr_t>(__readgsqword(0x58));
        const auto index = get<std::uint32_t>(base + addr::engine::tls_index);
        if (index > 4095) return;
        const auto tls = ptr(tls_array, std::uintptr_t(index) * 8);
        if (!get<std::uint8_t>(base + addr::engine::entity_creation_ready) || !get<std::uint8_t>(tls + 0xb19) || ptr(tls, 0x550) != context) {
            if (active) waiting(s, "waiting for verified native client creation scope");
            return;
        }
        if (!active) {
            bool tracked{};
            {
                std::lock_guard lock(s.mutex);
                tracked = !s.entities.empty();
            }
            if (tracked || !s.gates.empty()) clear(base);
            s.waiting.clear();
            return;
        }
        if (s.world != world || s.game != match.key) {
            clear(base);
            s.assets = {};
            s.world = world;
            s.game = match.key;
            s.error.clear();
        }
        s.context = context;
        if (!s.verified) {
            prefix(base, e::descriptor_init, e::descriptor_init_prefix);
            prefix(base, e::descriptor_destroy, e::descriptor_destroy_prefix);
            prefix(base, e::create_entity, e::create_entity_prefix);
            prefix(base, e::initialize_placement, e::initialize_placement_prefix);
            prefix(base, e::move_spatial_entity, e::move_spatial_entity_prefix);
            // ReSkate's shared entity lifetime hook already detours Destroy.
            prefix(base, b::create_bus, b::create_bus_prefix);
            prefix(base, b::acquire_bus, b::acquire_bus_prefix);
            std::string detail;
            require(multiplayer::install_entity_hooks(base, detail), "Native entity lifetime hooks unavailable.");
            multiplayer::set_entity_destroyed_listener(&entity_destroyed);
            s.verified = true;
        }
        if (!assets_resident(base, s)) {
            // The native registry owns the asset lifetime, not the local client context. Recreate
            // only after all current roots resolve again.
            clear(base);
            s.assets = {};
            s.asset_domains = {};
            s.retry = 0;
        }
        if (!std::all_of(s.assets.begin(), s.assets.end(), [](auto a) { return a != 0; })) {
            const auto now = GetTickCount64();
            if (now < s.retry) return;
            s.retry = now + 1000;
            if (!load_assets(base, s)) {
                waiting(s, std::format("waiting for checkpoint blueprints ({}/6 loaded)",
                                       std::count_if(s.assets.begin(), s.assets.end(), [](auto a) { return a != 0; })));
                return;
            }
        }
        // Gate 0 (the start) already has the native Throwdown flag; world checkpoint meshes begin
        // at index 1. The last point is the finish once it is placed and not still being aimed.
        std::vector<Gate> gates;
        const auto count = h.points.size();
        const bool still_placing = h.placing;
        for (std::size_t i = 1; i < count; ++i) {
            const bool finish = i + 1 == count && count >= 2 && !still_placing;
            const bool preview = still_placing && i + 1 == count;
            if (preview) continue; // the gate being aimed is not yet real: no native object for it
            gates.push_back({h.points[i], i < h.point_yaws.size() ? h.point_yaws[i] : 0.f,
                             std::clamp(i < h.point_widths.size() ? h.point_widths[i] : h.radius, 1.5f, 40.0f), finish});
        }
        if (gates.empty()) waiting(s, h.placing ? "waiting for a valid placement surface" : "waiting for route gates");
        std::set<unsigned> dirty;
        std::vector<unsigned> live_parts(gates.size());
        {
            std::lock_guard lock(s.mutex);
            for (const auto &entity : s.entities)
                if (entity.group < live_parts.size()) ++live_parts[entity.group];
            dirty = detail::pending_checkpoint_groups(s.dirty, live_parts);
        }
        if (gates == s.gates && dirty.empty()) return;
        const auto now = GetTickCount64();
        if (now < s.preview_at) return;
        s.preview_at = now + 16;
        const auto parent = parent_bus(base, context);
        if (!parent) {
            waiting(s, "waiting for the native local skater entity bus");
            return;
        }
        {
            std::lock_guard lock(s.mutex);
            std::fill(live_parts.begin(), live_parts.end(), 0);
            for (const auto &entity : s.entities)
                if (entity.group < live_parts.size()) ++live_parts[entity.group];
            dirty = detail::take_checkpoint_groups(s.dirty, live_parts);
        }
        for (unsigned i = 0; i < std::max(gates.size(), s.gates.size()); ++i) {
            if (i < gates.size() && i < s.gates.size() && gates[i] == s.gates[i] && !dirty.contains(i)) continue;
            // Move a resident arch in place. Recreating all four meshes several times a second made
            // the preview jump and restart its effects.
            if (i < gates.size() && i < s.gates.size() && gates[i].finish == s.gates[i].finish && !dirty.contains(i)) {
                std::vector<Entity> group;
                {
                    std::lock_guard lock(s.mutex);
                    for (const auto &entity : s.entities)
                        if (entity.group == i) group.push_back(entity);
                }
                if (group.size() == 4 && std::all_of(group.begin(), group.end(), [](const auto &entity) {
                        const auto collection = ptr(entity.entity, 0x70);
                        return ptr(entity.entity, 0x20) == entity.context && ptr(entity.entity, 0x40) == entity.owner && collection &&
                               ptr(collection) == entity.entity;
                    })) {
                    for (const auto &entity : group) {
                        const auto matrix = mesh_pose(gates[i], entity.part);
                        reinterpret_cast<void (*)(std::uintptr_t, const void *, std::uint8_t, std::uint8_t)>(base + e::move_spatial_entity)(
                            entity.entity, matrix.data(), 1, 1);
                    }
                    continue;
                }
            }
            clear_group(base, i);
            if (i < gates.size())
                for (unsigned part = 0; part < 4; ++part) {
                    const auto &gate = gates[i];
                    (void)create(base, parent, s.assets[(gate.finish ? 3 : 0) + (part < 2 ? part : 2)], mesh_pose(gate, part), i, part);
                }
        }
        s.gates = gates;
        if (!gates.empty()) waiting(s, "native checkpoint entities created");
    } catch (const std::exception &e0) {
        if (s.error != e0.what()) {
            s.error = e0.what();
            logging::log(logging::Level::error, logging::Channel::runtime, "Deathrace objects: {}", s.error);
        }
    } catch (...) {
        logging::write(logging::Level::error, logging::Channel::runtime, "Deathrace object creation failed.");
    }
}
} // namespace dingosdk::modes::race_world
