#include "bone_cam.h"
#include "bone_sprites.h"
#include "skater_skeleton.h"
#include "Extension/Console/commands.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/UI/game_view.h"
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <cstring>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <limits>
#include <mutex>
#include <span>

namespace dingosdk::modes {
namespace {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;
constexpr std::uint64_t duration_ms = 4200, fade_in_ms = 150, fade_out_ms = 600, settle_ms = 150;
// Marks on the ground fade after half a minute, a hit flashes briefly.
constexpr std::uint64_t mark_ms = 30000, hit_ms = 450;
constexpr std::size_t max_marks = 24, max_mark_points = 64;
constexpr std::uint16_t head_up = 0xffff; // the skull points along the head's own up axis

// The animation pose is parent-local {scale, Hamilton quaternion, translation} per joint, and the
// root chain carries the skater's world placement (client_first_person.cpp), so composing from
// the root lands in world space.
struct Joint {
    Quat rotation{0, 0, 0, 1};
    Vec3 position{};
    float scale = 1;
};
Vec3 rotate(const Quat &q, const Vec3 &v) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
            v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
Joint child(const Joint &parent, const std::array<float, 12> &bone) {
    const auto &q = parent.rotation;
    const auto offset = rotate(q, {bone[8] * parent.scale, bone[9] * parent.scale, bone[10] * parent.scale});
    Joint result;
    for (std::size_t i = 0; i < 3; ++i) result.position[i] = parent.position[i] + offset[i];
    result.rotation = {q[3] * bone[4] + q[0] * bone[7] + q[1] * bone[6] - q[2] * bone[5],
                       q[3] * bone[5] - q[0] * bone[6] + q[1] * bone[7] + q[2] * bone[4],
                       q[3] * bone[6] + q[0] * bone[5] - q[1] * bone[4] + q[2] * bone[7],
                       q[3] * bone[7] - q[0] * bone[4] - q[1] * bone[5] - q[2] * bone[6]};
    result.scale = parent.scale * (bone[0] + bone[1] + bone[2]) / 3.0f;
    return result;
}

// Each bone sprite between two joints.
struct Span {
    BoneSprite sprite;
    std::uint16_t a, b;
};
constexpr Span spans[]{
    {BoneSprite::torso, joint::hips, joint::head}, // the torso art runs up the neck to the skull
    {BoneSprite::femur_r, joint::right_up_leg, joint::right_leg}, {BoneSprite::femur_l, joint::left_up_leg, joint::left_leg},
    {BoneSprite::shin_r, joint::right_leg, joint::right_foot},    {BoneSprite::shin_l, joint::left_leg, joint::left_foot},
    {BoneSprite::foot_r, joint::right_foot, joint::right_toe},    {BoneSprite::foot_l, joint::left_foot, joint::left_toe},
    {BoneSprite::humerus_r, joint::right_arm, joint::right_fore_arm}, {BoneSprite::humerus_l, joint::left_arm, joint::left_fore_arm},
    {BoneSprite::forearm_r, joint::right_fore_arm, joint::right_hand}, {BoneSprite::forearm_l, joint::left_fore_arm, joint::left_hand},
    {BoneSprite::hand_r, joint::right_hand, joint::right_middle}, {BoneSprite::hand_l, joint::left_hand, joint::left_middle},
    {BoneSprite::skull, joint::head, head_up},
};
// A joint that stops dead hurts the bone beside it: cracked (or sprained, torn...) past `crack`
// m/s of speed lost into what it hit, broken past `snap`. Each bone has its own limits, roughly as
// bodies go: ankles, wrists and collarbones give first; the femur and pelvis take the most.
struct Watch {
    std::uint16_t joint;
    BoneSprite sprite;
    const char *cracked_bone, *cracked, *broken_bone, *broken;
    float crack, snap;
};
constexpr Watch watches[]{
    {joint::head, BoneSprite::skull, "SKULL", "CONCUSSION", "SKULL", "FRACTURED", 4.0f, 7.5f},
    {joint::right_hand, BoneSprite::forearm_r, "RIGHT WRIST", "SPRAINED", "RIGHT RADIUS", "SNAPPED", 4.5f, 7.5f},
    {joint::left_hand, BoneSprite::forearm_l, "LEFT WRIST", "SPRAINED", "LEFT RADIUS", "SNAPPED", 4.5f, 7.5f},
    {joint::right_fore_arm, BoneSprite::humerus_r, "RIGHT ELBOW", "DISLOCATED", "RIGHT HUMERUS", "FRACTURED", 5.5f, 9.0f},
    {joint::left_fore_arm, BoneSprite::humerus_l, "LEFT ELBOW", "DISLOCATED", "LEFT HUMERUS", "FRACTURED", 5.5f, 9.0f},
    {joint::right_arm, BoneSprite::torso, "RIGHT COLLARBONE", "CRACKED", "RIGHT COLLARBONE", "BROKEN", 5.0f, 8.0f},
    {joint::left_arm, BoneSprite::torso, "LEFT COLLARBONE", "CRACKED", "LEFT COLLARBONE", "BROKEN", 5.0f, 8.0f},
    {joint::spine2, BoneSprite::torso, "RIBS", "CRACKED", "RIBS", "BROKEN", 6.0f, 9.5f},
    {joint::hips, BoneSprite::torso, "PELVIS", "BRUISED", "PELVIS", "FRACTURED", 7.0f, 11.0f},
    {joint::right_leg, BoneSprite::femur_r, "RIGHT KNEE", "TORN", "RIGHT FEMUR", "FRACTURED", 6.5f, 10.5f},
    {joint::left_leg, BoneSprite::femur_l, "LEFT KNEE", "TORN", "LEFT FEMUR", "FRACTURED", 6.5f, 10.5f},
    {joint::right_foot, BoneSprite::shin_r, "RIGHT ANKLE", "SPRAINED", "RIGHT TIBIA", "SNAPPED", 5.0f, 8.0f},
    {joint::left_foot, BoneSprite::shin_l, "LEFT ANKLE", "SPRAINED", "LEFT TIBIA", "SNAPPED", 5.0f, 8.0f},
};
// A concussion's grade from the hardest hit the head took (m/s lost), and how long its after-
// effects last. Below the first, no concussion.
struct Concussion {
    float from;
    const char *what;
    std::uint64_t lasts_ms;
};
constexpr Concussion concussions[]{
    {4.0f, "MILD CONCUSSION", 4000}, {5.5f, "CONCUSSION", 7000}, {7.0f, "SEVERE CONCUSSION", 10000}, {8.5f, "KNOCKED OUT", 12000}};
constexpr std::size_t watch_count = std::size(watches);

enum class Setting { meat, on, off };
struct Mark {
    std::vector<Vec3> path;
    std::uint64_t born{}, last{};
    std::size_t watch{};
    bool skid{};
};
struct State {
    Setting setting = Setting::meat;
    bool effects = true;
    std::uint64_t effects_until{};
    std::vector<Mark> marks;
    float hit{};
    std::uint64_t hit_time{};
    Vec3 hit_at{};
    float head_hit{}; // the hardest hit the head took this bail (m/s lost): its concussion
    bool rang{};      // the concussion's ringing played this bail
    bool active{}, preview{}, wipeouts_known{}, have_position{}, have_velocity{}, pose_reported{}, posed{};
    std::uint64_t started{}, serial{}, wipeouts{}, last_tick{}, unready_since{};
    std::array<Vec3, watch_count> position{}, velocity{};
    // The speed each joint carried lately (decaying over a third of a second): into the ground,
    // and along it. A bail is often noticed only once the body has already hit, so the hit is
    // measured against what the joint carried before, not just the last tick.
    std::array<float, watch_count> carry_down{}, carry_side{};
    // The hips' recent speeds, to notice the body stopping dead (the ground, a pole, a wall).
    std::array<std::pair<std::uint64_t, float>, 24> hips_speeds{};
    std::size_t hips_next{};
    std::atomic<bool> slam_seen{};
    std::uint64_t slam_time{};
    std::array<std::uint8_t, watch_count> level{};
    std::array<std::uint8_t, static_cast<std::size_t>(BoneSprite::count)> hurt{};
    std::vector<Joint> joints = std::vector<Joint>(skater_joint_count);
    std::vector<std::array<float, 12>> raw = std::vector<std::array<float, 12>>(skater_joint_count);
    std::atomic<bool> bail_pending{};
    std::mutex mutex;
    overlay::BoneCam shown;
};
State &state() {
    static auto *value = new State;
    return *value;
}
const Concussion *concussion_of(float head_hit) {
    const Concussion *found{};
    for (const auto &c : concussions)
        if (head_hit >= c.from) found = &c;
    return found;
}
// A concussion's ears ringing: one high tone, very quiet, fading over a few seconds.
void ring(float strength) {
    static std::vector<std::uint8_t> wav;
    constexpr std::uint32_t rate = 22050;
    const float seconds = 1.5f + 2.0f * strength, volume = 0.02f + 0.025f * strength;
    const auto count = static_cast<std::uint32_t>(seconds * rate), bytes = count * 2;
    PlaySoundW(nullptr, nullptr, 0); // the buffer below may be playing: stop it before it changes
    wav.assign(44 + bytes, 0);
    const auto put = [&](std::size_t at, std::uint32_t value, int size) {
        for (int i = 0; i < size; ++i) wav[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    std::memcpy(wav.data(), "RIFF", 4);
    put(4, 36 + bytes, 4);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put(16, 16, 4);       // format block size
    put(20, 1, 2);        // PCM
    put(22, 1, 2);        // mono
    put(24, rate, 4);
    put(28, rate * 2, 4); // bytes per second
    put(32, 2, 2);
    put(34, 16, 2);
    std::memcpy(wav.data() + 36, "data", 4);
    put(40, bytes, 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / rate;
        const float envelope = std::min(1.0f, t / 0.4f) * std::max(0.0f, 1.0f - t / seconds);
        const float tone = std::sin(6.2831853f * 3800.0f * t) * (0.85f + 0.15f * std::sin(6.2831853f * 3.0f * t));
        put(44 + i * 2, static_cast<std::uint16_t>(static_cast<std::int16_t>(tone * envelope * volume * 32767)), 2);
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
std::uint64_t now_ms() { return GetTickCount64(); }


void start(State &s, std::uint64_t now) {
    s.active = true;
    // Joint speeds tracked up to now carry over, so the hit that set off the bail still counts.
    if (now - s.last_tick > 100) {
        s.have_position = s.have_velocity = false;
        s.carry_down.fill(0);
        s.carry_side.fill(0);
        s.last_tick = now;
    }
    s.started = now;
    ++s.serial;
    s.head_hit = 0;
    s.rang = false;
    s.level.fill(0);
    s.hurt.fill(0);
    s.preview = false;
    s.pose_reported = false;
    s.posed = false;
    s.effects_until = now + mark_ms;
    logging::log(logging::Level::info, logging::Channel::runtime, "Bone Cam: started.");
}
// Ends the X-ray and everything the slams left behind.
void stop(State &s) {
    s.active = false;
    s.effects_until = 0;
    s.marks.clear();
    s.hit = 0;
    std::lock_guard lock(s.mutex);
    s.shown = {};
}
// A joint sliding along the ground leaves a mark: its own one, carried on while it keeps sliding.
void leave_mark(State &s, std::size_t w, const Vec3 &at, std::uint64_t now) {
    Mark *mark{};
    for (auto &m : s.marks)
        if (m.watch == w && now - m.last <= 250) mark = &m;
    if (!mark) {
        if (s.marks.size() >= max_marks) s.marks.erase(s.marks.begin());
        const auto j = watches[w].joint;
        s.marks.push_back({{}, now, now, w, j == joint::hips || j == joint::spine2});
        mark = &s.marks.back();
    }
    mark->last = now;
    if (!mark->path.empty()) {
        const auto &end = mark->path.back();
        if (std::hypot(at[0] - end[0], at[2] - end[2]) < 0.12f) return;
    }
    if (mark->path.size() < max_mark_points) mark->path.push_back(at);
}
// The skater's pose this tick, composed into world space. Empty while it can be read, else why not.
std::string read_pose(State &s, std::uintptr_t base, std::uintptr_t client) {
    try {
        namespace detail = client_source::detail;
        const auto component = detail::first_person_component(base, client);
        std::uintptr_t holder{};
        if (!detail::first_person_read(component + 0xa0, &holder, 8) || !holder) return "no animation holder";
        const auto pose = multiplayer::read_native_pose_layout(detail::first_person_read, base, holder, 512);
        if (!pose.buffer) return "no pose buffer";
        if (pose.count != skater_joint_count) return "the skeleton has " + std::to_string(pose.count) + " joints";
        if (!detail::first_person_read(pose.buffer, s.raw.data(), skater_joint_count * sizeof(s.raw[0]))) return "pose unreadable";
        for (std::size_t i = 0; i < skater_joint_count; ++i) {
            const auto &bone = s.raw[i];
            for (const std::size_t lane : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
                if (!std::isfinite(bone[lane]) || std::abs(bone[lane]) > 1.0e6f) return "pose not ready";
            const auto parent = skater_joint_parent[i];
            s.joints[i] = child(parent < 0 ? Joint{} : s.joints[static_cast<std::size_t>(parent)], bone);
        }
        return {};
    } catch (const client_source::detail::SourceGuard &guard) {
        return guard.message ? guard.message : "skater unavailable";
    } catch (const std::exception &e) {
        return e.what();
    } catch (...) {
        return "unknown error";
    }
}
void injure(State &s, std::size_t w, std::uint8_t level) {
    if (level <= s.level[w]) return;
    s.level[w] = level;
    auto &hurt = s.hurt[static_cast<std::size_t>(watches[w].sprite)];
    hurt = std::max(hurt, level);
}
// Joints whose velocity changed sharply since the last tick hit something.
// Joints that hit the ground hard. Only the parts of the body touching the ground (the lowest at
// that moment) take damage, and only for the speed they were carrying into it and lost: a
// head-first slam cracks the skull and collarbones, a feet-first one the ankles and shins, and a
// ragdoll's flailing hurts nothing. The skull gives first; at most three bones break per bail.
// Outside a bail (`injuring` false) it only keeps the joints' speeds, and watches the hips for a
// dead stop: a body that loses 7 m/s within a third of a second hit something (bone_cam_slam).
void find_impacts(State &s, std::uint64_t now, bool injuring = true) {
    const float seconds = static_cast<float>(now - s.last_tick) / 1000.0f;
    s.last_tick = now;
    const bool usable = seconds > 0.004f && seconds < 0.1f;
    float lowest = std::numeric_limits<float>::max();
    for (const auto &w : watches) lowest = std::min(lowest, s.joints[w.joint].position[1]);
    auto broken = static_cast<std::size_t>(std::count(s.level.begin(), s.level.end(), std::uint8_t{2}));
    const float decay = usable ? std::exp(-seconds / 0.33f) : 0.0f;
    for (std::size_t w = 0; w < watch_count; ++w) {
        const auto &p = s.joints[watches[w].joint].position;
        if (s.have_position && usable) {
            Vec3 v{};
            for (int i = 0; i < 3; ++i) v[i] = (p[i] - s.position[w][i]) / seconds;
            const float down = std::max(0.0f, -v[1]), side = std::hypot(v[0], v[2]);
            // A teleport or a reset is not a hit.
            if (down > 60.0f || side > 60.0f) {
                s.position[w] = p;
                s.carry_down[w] = s.carry_side[w] = 0;
                continue;
            }
            if (watches[w].joint == joint::hips) {
                const float speed = std::hypot(side, v[1]);
                float fastest = 0;
                for (const auto &[at, was] : s.hips_speeds)
                    if (at && now - at <= 330) fastest = std::max(fastest, was);
                if (fastest - speed >= 7.0f) {
                    s.slam_seen.store(true);
                    s.slam_time = now;
                }
                s.hips_speeds[s.hips_next++ % s.hips_speeds.size()] = {now, speed};
            }
            // Joints touching the ground, or any joint while the whole body stops dead (a pole, a wall).
            const bool touching = p[1] - lowest <= 0.3f || (s.slam_time && now - s.slam_time <= 150);
            if (injuring && s.have_velocity && touching) {
                // The speed it carried into the ground and lost, plus some of the speed along it
                // lost (a scrape or a wall).
                const float into = std::max(0.0f, s.carry_down[w] - down);
                const float sideways = std::max(0.0f, s.carry_side[w] - side);
                const float impact = into + 0.35f * sideways;
                // The preview shows the skeleton alone: nothing cracks or breaks in it.
                if (!s.preview) {
                    if (impact >= watches[w].snap && broken < 3) {
                        if (s.level[w] < 2) ++broken; // the cap holds within one tick too
                        injure(s, w, 2);
                    } else if (impact >= watches[w].crack) {
                        injure(s, w, 1);
                    }
                }
                if (!s.preview && watches[w].joint == joint::head) s.head_hit = std::max(s.head_hit, impact);
                // Spent: the same hit is not counted again next tick.
                if (impact >= 3.0f) {
                    s.carry_down[w] = down;
                    s.carry_side[w] = side;
                }
                if (!s.preview && impact >= 3.0f) {
                    // The hit's flash, brighter the harder the hit.
                    const float since = static_cast<float>(now - s.hit_time) / hit_ms;
                    const float left = s.hit * std::max(0.0f, 1.0f - since), strength = std::clamp(impact / 9.0f, 0.3f, 1.0f);
                    if (strength > left) {
                        s.hit = strength;
                        s.hit_time = now;
                        s.hit_at = p;
                    }
                }
            }
            // Sliding along the ground: marks under the joint (joint centres sit about 0.1 m in the body).
            if (injuring && s.effects && !s.preview && p[1] - lowest <= 0.15f && now - s.started >= settle_ms && side > 1.5f)
                leave_mark(s, w, {p[0], lowest - 0.08f, p[2]}, now);
            s.carry_down[w] = std::max(s.carry_down[w] * decay, down);
            s.carry_side[w] = std::max(s.carry_side[w] * decay, side);
            s.velocity[w] = v;
        }
        s.position[w] = p;
    }
    if (usable) s.have_velocity = s.have_position;
    s.have_position = true;
}
// The joints the 3D X-ray builds its bones on. The spine between the hips and the head, and the
// collarbones (the arms' parents), are found once from the skeleton's parent table.
struct PoseJoints {
    std::vector<std::uint16_t> spine; // hips .. head
    std::array<std::uint16_t, 2> clavicle{};
};
const PoseJoints &pose_joints() {
    static const PoseJoints value = [] {
        PoseJoints p;
        for (int j = joint::head; j >= 0 && j != joint::hips; j = skater_joint_parent[static_cast<std::size_t>(j)])
            p.spine.insert(p.spine.begin(), static_cast<std::uint16_t>(j));
        p.spine.insert(p.spine.begin(), joint::hips);
        for (std::size_t side = 0; side < 2; ++side) {
            const auto arm = side ? joint::left_arm : joint::right_arm;
            const auto parent = skater_joint_parent[arm];
            p.clavicle[side] = static_cast<std::uint16_t>(parent >= 0 ? parent : arm);
        }
        return p;
    }();
    return value;
}
overlay::BoneCamPose pose_of(const State &s) {
    overlay::BoneCamPose pose;
    const auto &p = pose_joints();
    const auto at = [&](std::uint16_t j) { return s.joints[j].position; };
    for (const auto j : p.spine) pose.spine.push_back(at(j));
    pose.head = at(joint::head);
    pose.head_up = rotate(s.joints[joint::head].rotation, {1, 0, 0}); // the head joint's local +X is up
    for (std::size_t side = 0; side < 2; ++side) {
        const bool l = side == 1;
        pose.clavicle[side] = at(p.clavicle[side]);
        pose.shoulder[side] = at(l ? joint::left_arm : joint::right_arm);
        pose.elbow[side] = at(l ? joint::left_fore_arm : joint::right_fore_arm);
        pose.wrist[side] = at(l ? joint::left_hand : joint::right_hand);
        pose.fingers[side] = at(l ? joint::left_middle : joint::right_middle);
        pose.hip[side] = at(l ? joint::left_up_leg : joint::right_up_leg);
        pose.knee[side] = at(l ? joint::left_leg : joint::right_leg);
        pose.ankle[side] = at(l ? joint::left_foot : joint::right_foot);
        pose.toe[side] = at(l ? joint::left_toe : joint::right_toe);
    }
    pose.hurt.assign(s.hurt.begin(), s.hurt.end());
    pose.valid = true;
    return pose;
}

// The marks and hit as they look now: marks fade over their last 8 s.
void publish_effects(State &s, std::uint64_t now, overlay::BoneCam &cam) {
    if (!s.effects || s.preview) return;
    std::erase_if(s.marks, [&](const Mark &m) { return now - m.born >= mark_ms; });
    for (const auto &m : s.marks) {
        if (m.path.size() < 2) continue;
        const float age = static_cast<float>(now - m.born);
        cam.marks.push_back({m.path, std::clamp((mark_ms - age) / 8000.0f, 0.0f, 1.0f), m.skid});
    }
    const float since = static_cast<float>(now - s.hit_time) / hit_ms;
    if (s.hit > 0 && since < 1) {
        cam.hit = s.hit * (1 - since) * (1 - since);
        cam.hit_at = s.hit_at;
    }
    // A concussion's after-effects, once the X-ray is over: kept faint (a soft dimming at the
    // edges, a slow sway of it), strongest at once and easing off over the concussion's time.
    if (const auto *c = concussion_of(s.head_hit); c && now >= s.started + duration_ms) {
        const auto into = now - (s.started + duration_ms);
        if (into < c->lasts_ms) {
            const float grade = std::clamp((s.head_hit - 4.0f) / 5.0f, 0.0f, 1.0f);
            cam.daze = (0.35f + 0.65f * grade) * (1.0f - static_cast<float>(into) / c->lasts_ms);
            if (!s.rang) {
                s.rang = true;
                ring(grade);
            }
        }
    }
    cam.effects = !cam.marks.empty() || cam.hit > 0 || cam.daze > 0;
}
void publish(State &s, std::uint64_t now, bool bones = true) {
    overlay::BoneCam cam;
    publish_effects(s, now, cam);
    cam.active = s.active;
    if (!s.active) {
        std::lock_guard lock(s.mutex);
        s.shown = std::move(cam);
        return;
    }
    cam.serial = s.serial;
    cam.preview = s.preview;
    const auto elapsed = now - s.started, left = duration_ms > elapsed ? duration_ms - elapsed : 0;
    cam.fade = std::clamp(std::min(elapsed / static_cast<float>(fade_in_ms), left / static_cast<float>(fade_out_ms)), 0.0f, 1.0f);
    const auto &l = s.joints[joint::left_arm].position, &r = s.joints[joint::right_arm].position;
    cam.left = {l[0] - r[0], l[1] - r[1], l[2] - r[2]};
    if (bones) cam.pose = pose_of(s);
    for (const auto &span : bones ? std::span<const Span>(spans) : std::span<const Span>()) {
        overlay::BoneCamBone bone;
        bone.sprite = static_cast<std::uint8_t>(span.sprite);
        bone.a = s.joints[span.a].position;
        if (span.b == head_up) {
            // The head joint's local +X is up (client_first_person.cpp).
            const auto up = rotate(s.joints[span.a].rotation, {1, 0, 0});
            for (int i = 0; i < 3; ++i) bone.b[i] = bone.a[i] + up[i] * 0.2f;
        } else {
            bone.b = s.joints[span.b].position;
        }
        bone.hurt = s.hurt[static_cast<std::size_t>(span.sprite)];
        cam.bones.push_back(bone);
    }
    for (std::size_t w = 0; w < watch_count; ++w) {
        if (watches[w].joint == joint::head) continue; // the head is listed by its concussion below
        if (s.level[w])
            cam.injuries.push_back({s.level[w] == 2 ? watches[w].broken_bone : watches[w].cracked_bone,
                                    s.level[w] == 2 ? watches[w].broken : watches[w].cracked, s.level[w] == 2});
    }
    // The head: a fractured skull, and the concussion graded by how hard it hit.
    if (s.level[0] == 2) cam.injuries.push_back({"SKULL", "FRACTURED", true});
    if (const auto *c = concussion_of(s.head_hit)) cam.injuries.push_back({"HEAD", c->what, c->from >= 7.0f});
    std::stable_sort(cam.injuries.begin(), cam.injuries.end(), [](const auto &a, const auto &b) { return a.severe > b.severe; });
    std::lock_guard lock(s.mutex);
    s.shown = std::move(cam);
}
} // namespace

void tick_bone_cam(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept {
    try {
        auto &s = state();
        const auto now = now_ms();
        // Only a lasting loss of play ends it: a slam can briefly take the game out of play.
        if (!playing) {
            if (!s.unready_since) s.unready_since = now;
            if (!s.active || now - s.unready_since >= 2000) stop(s);
            return;
        }
        s.unready_since = 0;
        // Bails are found by the game modes (game_modes.cpp), which know every wipeout state.
        const bool wanted = s.setting == Setting::on || (s.setting == Setting::meat && local_meat_game());
        if (s.bail_pending.exchange(false) && wanted && !s.active) start(s, now);
        // The X-ray ends after a few seconds; the marks stay a while longer.
        if (s.active && now - s.started >= duration_ms) s.active = false;
        const bool showing = s.active || (s.effects && now < s.effects_until);
        if (!showing) {
            {
                std::lock_guard lock(s.mutex);
                s.shown = {};
            }
            // Between bails the joints are still followed, so the hit that sets off a bail counts.
            // Hall of Meat needs them with the Bone Cam off too: its on-foot slams (bone_cam_slam)
            // come from here.
            if ((wanted || local_meat_game()) && read_pose(s, base, client).empty()) find_impacts(s, now, false);
            return;
        }
        if (const auto why = read_pose(s, base, client); !why.empty()) {
            if (!s.pose_reported) {
                s.pose_reported = true;
                logging::log(logging::Level::warning, logging::Channel::runtime, "Bone Cam: cannot read the skater's pose: {}", why);
            }
            // Mid-ragdoll the skater can be out of reach: the X-ray and injuries still show, with
            // the last pose read this bail if there was one.
            publish(s, now, s.posed);
            return;
        }
        s.posed = true;
        if (!s.pose_reported) {
            s.pose_reported = true;
            logging::log(logging::Level::info, logging::Channel::runtime, "Bone Cam: reading the skater's {} joints.", skater_joint_count);
        }
        find_impacts(s, now, s.active);
        publish(s, now);
    } catch (...) {}
}

void bone_cam_bail() noexcept { state().bail_pending.store(true); }

bool bone_cam_slam() noexcept { return state().slam_seen.exchange(false); }

std::string bone_cam_setting() {
    switch (state().setting) {
    case Setting::on: return "on";
    case Setting::off: return "off";
    default: return "meat";
    }
}

std::string bone_cam_command(const std::vector<std::string> &arguments) {
    auto &s = state();
    const auto word = arguments.empty() ? std::string() : arguments[0];
    if (word.empty())
        return "Bone Cam: " + bone_cam_setting() + ", slam effects " + (s.effects ? "on" : "off") +
               " (mode bonecam meat|on|off|test, mode bonecam effects on|off).";
    if (word == "effects") {
        const auto value = arguments.size() > 1 ? arguments[1] : std::string();
        if (value != "on" && value != "off") return "error: usage: mode bonecam effects on|off";
        s.effects = value == "on";
        if (!s.effects) {
            s.marks.clear();
            s.hit = 0;
        }
        return s.effects ? "Slam effects on: skid marks, scrapes and the hit flash." : "Slam effects off.";
    }
    if (word == "meat") s.setting = Setting::meat;
    else if (word == "on") s.setting = Setting::on;
    else if (word == "off") { s.setting = Setting::off; stop(s); }
    else if (word == "test") {
        // A look at the Bone Cam without bailing: the skeleton alone, nothing broken.
        stop(s);
        start(s, now_ms());
        s.preview = true;
        return "Bone Cam preview for a few seconds.";    } else {
        return "error: usage: mode bonecam meat|on|off|test";
    }
    return word == "meat" ? "Bone Cam shows in Hall of Meat games." : word == "on" ? "Bone Cam shows on every bail." : "Bone Cam off.";
}

overlay::BoneCam bone_cam() {
    auto &s = state();
    overlay::BoneCam cam;
    {
        std::lock_guard lock(s.mutex);
        if (!s.shown.active && !s.shown.effects) return {};
        cam = s.shown;
    }
    if (const auto view = latest_game_view()) {
        cam.camera = view->world;
        cam.vertical_fov = view->vertical_fov;
    }
    return cam;
}
} // namespace dingosdk::modes
