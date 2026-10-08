#include "bone_cam_3d.h"
#include "Extension/Modes/bone_sprites.h"
#include <algorithm>
#include <cmath>
#include <vector>

// Bones are generated around the joints every frame (no mesh assets): tubes whose radius swells
// at both ends for the long bones, ellipsoids for the skull, jaw, vertebrae, pelvis and shoulder
// blades, and thin curved tubes for the ribs. Each triangle is projected with the game's camera,
// lit from just above the camera with a bright rim at its silhouette (an X-ray is densest where
// it looks through the edge of a bone), and drawn far to near. A flesh shell (capsules around the
// limbs, an ellipsoid torso and head) goes first, nearly clear but for its rim, so the bones read
// as inside the skater. Hurt regions tint orange (cracked) or red (broken).

namespace dingosdk::overlay::detail {
namespace {
using V = std::array<float, 3>;
using Sprite = modes::BoneSprite;
V operator+(const V &a, const V &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V operator-(const V &a, const V &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V operator*(const V &a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot(const V &a, const V &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V &a, const V &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(const V &a) { return std::sqrt(dot(a, a)); }
V unit(const V &a, const V &fallback = {0, 1, 0}) {
    const float l = length(a);
    return l > 1e-5f ? a * (1.0f / l) : fallback;
}
V lerp(const V &a, const V &b, float t) { return a + (b - a) * t; }
// Two unit vectors at right angles to `axis` (and each other).
void frame(const V &axis, V &p, V &q) {
    const V helper = std::abs(axis[1]) < 0.9f ? V{0, 1, 0} : V{1, 0, 0};
    p = unit(cross(axis, helper));
    q = cross(axis, p);
}

struct Material {
    float r, g, b;  // 0..1
    float alpha;    // bone: opaque-ish; flesh: scaled by the rim
    bool flesh{};
};
constexpr Material bone{0.80f, 0.90f, 1.00f, 0.95f}, cracked{1.00f, 0.66f, 0.32f, 0.95f}, broken{1.00f, 0.32f, 0.26f, 0.97f},
                   socket{0.02f, 0.05f, 0.12f, 0.95f}, flesh{0.35f, 0.65f, 0.95f, 1.0f, true};

struct Tri {
    ImVec2 p[3];
    float depth;
    ImU32 colour;
};
struct Builder {
    const XrayView &view;
    float fade;
    std::vector<Tri> tris;
    V light;
    explicit Builder(const XrayView &v, float f) : view(v), fade(f) {
        // From above the camera and a little to its left: the shapes read as round.
        light = unit(view.back * 0.75f + view.up * 0.55f - view.right * 0.35f);
    }
    bool project(const V &p, ImVec2 &out, float &depth) const {
        const V d = p - view.origin;
        depth = -dot(d, view.back);
        if (depth < 0.08f) return false;
        out = ImVec2(view.centre.x + dot(d, view.right) * view.focal / depth, view.centre.y - dot(d, view.up) * view.focal / depth);
        return true;
    }
    void tri(const V &a, const V &b, const V &c, const Material &m) {
        Tri t{};
        float da{}, db{}, dc{};
        if (!project(a, t.p[0], da) || !project(b, t.p[1], db) || !project(c, t.p[2], dc)) return;
        const V centre = (a + b + c) * (1.0f / 3.0f);
        V n = unit(cross(b - a, c - a), view.back);
        const V eye = unit(view.origin - centre);
        if (dot(n, eye) < 0) n = n * -1.0f; // both sides lit as the side facing the camera
        const float facing = std::clamp(dot(n, eye), 0.0f, 1.0f), rim = (1 - facing) * (1 - facing);
        const float diffuse = std::max(0.0f, dot(n, light));
        float r{}, g{}, bl{}, alpha{};
        if (m.flesh) {
            r = m.r;
            g = m.g;
            bl = m.b;
            alpha = (0.03f + 0.30f * rim) * fade;
        } else {
            const float shade = 0.28f + 0.62f * diffuse;
            r = std::min(1.0f, m.r * shade + 0.55f * rim);
            g = std::min(1.0f, m.g * shade + 0.65f * rim);
            bl = std::min(1.0f, m.b * shade + 0.75f * rim);
            alpha = m.alpha * fade;
        }
        t.depth = (da + db + dc) / 3.0f;
        t.colour = IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(bl * 255),
                            static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255));
        tris.push_back(t);
    }
    void quad(const V &a, const V &b, const V &c, const V &d, const Material &m) {
        tri(a, b, c, m);
        tri(a, c, d, m);
    }
    // A tube from a to b: radius r0 at a easing to r1 at b, swelling by `knob` at both ends
    // (a long bone's condyles and head).
    void tube(const V &a, const V &b, float r0, float r1, float knob, const Material &m, int sides = 10, int rings = 8) {
        const V axis = b - a;
        if (length(axis) < 0.005f) return;
        V p, q;
        frame(unit(axis), p, q);
        const auto radius = [&](float t) {
            return r0 + (r1 - r0) * t + knob * (std::exp(-(t / 0.12f) * (t / 0.12f)) + std::exp(-((1 - t) / 0.12f) * ((1 - t) / 0.12f)));
        };
        const auto at = [&](int ring, int side) {
            const float t = static_cast<float>(ring) / rings, angle = 6.2831853f * side / sides;
            return a + axis * t + (p * std::cos(angle) + q * std::sin(angle)) * radius(t);
        };
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < sides; ++j) quad(at(i, j), at(i, j + 1), at(i + 1, j + 1), at(i + 1, j), m);
        for (int j = 0; j < sides; ++j) { // closed ends
            tri(a, at(0, j + 1), at(0, j), m);
            tri(b, at(rings, j), at(rings, j + 1), m);
        }
    }
    // A tube along a path of points, the same thickness all along (ribs, the sternum).
    void path(const std::vector<V> &points, float r, const Material &m) {
        for (std::size_t i = 1; i < points.size(); ++i) tube(points[i - 1], points[i], r, r, 0, m, 6, 1);
    }
    // An ellipsoid: `x`, `y`, `z` its half-axes as vectors.
    void ellipsoid(const V &c, const V &x, const V &y, const V &z, const Material &m, int around = 14, int up = 9) {
        const auto at = [&](int i, int j) {
            const float phi = -1.5707963f + 3.1415926f * i / up, theta = 6.2831853f * j / around;
            return c + x * (std::cos(phi) * std::cos(theta)) + y * std::sin(phi) + z * (std::cos(phi) * std::sin(theta));
        };
        for (int i = 0; i < up; ++i)
            for (int j = 0; j < around; ++j) quad(at(i, j), at(i, j + 1), at(i + 1, j + 1), at(i + 1, j), m);
    }
    void draw(ImDrawList *list) {
        std::sort(tris.begin(), tris.end(), [](const Tri &a, const Tri &b) { return a.depth > b.depth; });
        for (const auto &t : tris)
            if ((t.colour >> IM_COL32_A_SHIFT) & 0xff) list->AddTriangleFilled(t.p[0], t.p[1], t.p[2], t.colour);
        tris.clear();
    }
};

const Material &material(int level) { return level >= 2 ? broken : level == 1 ? cracked : bone; }
// A long bone in the state it is in. Sound: whole. Cracked: a dark fracture line round its middle.
// Broken: snapped in two — a gap, the far half knocked sideways and bent off line, and jagged
// shards standing off both broken ends.
void long_bone(Builder &b, const V &a, const V &z, float r0, float r1, float knob, int level, int sides = 10, int rings = 8) {
    const auto &m = material(level);
    if (level < 2) {
        b.tube(a, z, r0, r1, knob, m, sides, rings);
        if (level == 1) {
            const V mid = lerp(a, z, 0.5f), axis = unit(z - a);
            const float r = (r0 + r1) * 0.5f;
            b.tube(mid - axis * 0.002f, mid + axis * 0.002f, r * 1.12f, r * 1.12f, 0, socket, sides, 1);
        }
        return;
    }
    const V axis = z - a, dir = unit(axis);
    V p, q;
    frame(dir, p, q);
    const float r = (r0 + r1) * 0.5f, gap = 0.012f;
    const V near_end = lerp(a, z, 0.5f) - dir * (gap * 0.5f);
    // The far half: shifted a bone's width sideways at the break and turned a few degrees.
    const V shift = p * (r * 1.6f) + q * (r * 0.5f);
    const V far_start = lerp(a, z, 0.5f) + dir * (gap * 0.5f) + shift, far_end = z + shift * 0.35f;
    b.tube(a, near_end, r0, r, knob, m, sides, rings / 2);
    b.tube(far_start, far_end, r, r1, knob, m, sides, rings / 2);
    // Splinters: thin spikes off both broken ends, leaning out.
    for (int i = 0; i < 4; ++i) {
        const float angle = 1.57f * i + 0.4f;
        const V out = p * std::cos(angle) + q * std::sin(angle);
        const float tall = 0.008f + 0.004f * (i % 2);
        b.tube(near_end + out * (r * 0.5f), near_end + dir * tall + out * (r * 0.9f), r * 0.35f, 0.0006f, 0, m, 4, 1);
        b.tube(far_start + out * (r * 0.5f), far_start - dir * tall + out * (r * 0.8f), r * 0.35f, 0.0006f, 0, m, 4, 1);
    }
}
// A point along a polyline, `t` of its length from its start.
V along(const std::vector<V> &line, float t) {
    float total = 0;
    for (std::size_t i = 1; i < line.size(); ++i) total += length(line[i] - line[i - 1]);
    float want = std::clamp(t, 0.0f, 1.0f) * total;
    for (std::size_t i = 1; i < line.size(); ++i) {
        const float piece = length(line[i] - line[i - 1]);
        if (want <= piece || i + 1 == line.size()) return lerp(line[i - 1], line[i], piece > 0 ? std::min(1.0f, want / piece) : 0);
        want -= piece;
    }
    return line.empty() ? V{} : line.back();
}
} // namespace

void draw_xray_skeleton(ImDrawList *draw, const BoneCamPose &pose, const XrayView &view, float fade) {
    if (!pose.valid || pose.spine.size() < 3 || fade <= 0) return;
    const auto level_of = [&](Sprite region) {
        const auto i = static_cast<std::size_t>(region);
        return i < pose.hurt.size() ? static_cast<int>(pose.hurt[i]) : 0;
    };
    const auto hurt = [&](Sprite region) -> const Material & { return material(level_of(region)); };
    const auto side_sprite = [](std::size_t side, Sprite right, Sprite left) { return side ? left : right; };

    // The torso's frame: up the spine, across the shoulders, and forward (the way the toes point,
    // which a ragdoll keeps roughly in front of its hips).
    const V hips = pose.spine.front(), neck = pose.spine[pose.spine.size() - 2];
    const V up = unit(neck - hips);
    V across = unit(pose.shoulder[1] - pose.shoulder[0], view.right); // toward the skater's left
    V forward = unit(cross(across, up), view.back * -1.0f);
    const V toes = (pose.toe[0] - pose.ankle[0]) + (pose.toe[1] - pose.ankle[1]);
    if (dot(forward, toes) < 0) forward = forward * -1.0f;
    across = unit(cross(up, forward)) * (dot(cross(up, forward), across) < 0 ? -1.0f : 1.0f);

    // Flesh first: everything inside it is drawn over it.
    Builder skin(view, fade);
    {
        const V torso_centre = along(pose.spine, 0.42f) + forward * 0.03f;
        skin.ellipsoid(torso_centre, across * 0.19f, up * (length(neck - hips) * 0.62f), forward * 0.13f, flesh, 16, 10);
        skin.ellipsoid(pose.head + pose.head_up * 0.09f, across * 0.11f, pose.head_up * 0.14f, forward * 0.12f, flesh, 14, 9);
        for (std::size_t s = 0; s < 2; ++s) {
            skin.tube(pose.hip[s], pose.knee[s], 0.085f, 0.06f, 0, flesh, 12, 4);
            skin.tube(pose.knee[s], pose.ankle[s], 0.06f, 0.04f, 0, flesh, 12, 4);
            skin.tube(pose.shoulder[s], pose.elbow[s], 0.055f, 0.045f, 0, flesh, 10, 3);
            skin.tube(pose.elbow[s], pose.wrist[s], 0.045f, 0.035f, 0, flesh, 10, 3);
            skin.tube(pose.wrist[s], pose.fingers[s] + (pose.fingers[s] - pose.wrist[s]) * 0.6f, 0.035f, 0.025f, 0, flesh, 8, 2);
            skin.tube(pose.ankle[s] - forward * 0.04f, pose.toe[s] + (pose.toe[s] - pose.ankle[s]) * 0.25f, 0.045f, 0.035f, 0, flesh, 8, 2);
        }
    }
    skin.draw(draw);

    Builder b(view, fade);
    // Skull and jaw, faced the way the torso faces, tipped with the head.
    {
        const V head_up = unit(pose.head_up, up);
        const V face = unit(forward - head_up * dot(forward, head_up), forward);
        const V side = unit(cross(head_up, face));
        const auto &m = hurt(Sprite::skull);
        const V cranium = pose.head + head_up * 0.095f - face * 0.01f;
        b.ellipsoid(cranium, side * 0.075f, head_up * 0.095f, face * 0.095f, m, 16, 11);
        b.ellipsoid(pose.head + head_up * 0.02f + face * 0.06f, side * 0.05f, head_up * 0.03f, face * 0.04f, m, 10, 6); // jaw
        for (const float s : {-1.0f, 1.0f}) // eye sockets
            b.ellipsoid(cranium + face * 0.085f + side * (0.032f * s) + head_up * 0.005f, side * 0.017f, head_up * 0.015f, face * 0.01f, socket, 8, 5);
        b.ellipsoid(cranium + face * 0.093f - head_up * 0.03f, side * 0.01f, head_up * 0.014f, face * 0.006f, socket, 6, 4); // nose
        // A cracked skull shows a fracture line over the crown; a broken one, a star of them.
        if (const int level = level_of(Sprite::skull); level > 0) {
            const auto on_skull = [&](float around, float height) { // a point just outside the cranium
                const float c = std::cos(height);
                return cranium + side * (0.077f * c * std::sin(around)) + head_up * (0.097f * std::sin(height)) + face * (0.097f * c * std::cos(around));
            };
            const int lines = level >= 2 ? 4 : 1;
            for (int l = 0; l < lines; ++l) {
                std::vector<V> seam;
                const float start = 0.4f + 1.6f * l;
                for (int i = 0; i <= 7; ++i)
                    seam.push_back(on_skull(start + 0.12f * i + ((i % 2) ? 0.07f : -0.07f), 1.25f - 0.13f * i));
                b.path(seam, level >= 2 ? 0.0028f : 0.0018f, socket);
            }
        }
    }
    // The spine: a vertebra every 3.5 cm from the pelvis to the skull, each with its spinous
    // process pointing back.
    {
        const auto &m = hurt(Sprite::torso);
        float total = 0;
        for (std::size_t i = 1; i < pose.spine.size(); ++i) total += length(pose.spine[i] - pose.spine[i - 1]);
        const int count = std::clamp(static_cast<int>(total / 0.035f), 8, 30);
        for (int i = 0; i < count; ++i) {
            const float t = 0.04f + 0.86f * i / (count - 1);
            const V c = along(pose.spine, t), next = along(pose.spine, std::min(1.0f, t + 0.02f));
            const V axis = unit(next - c, up);
            const float size = 0.02f - 0.008f * t; // smaller toward the neck
            b.ellipsoid(c, across * size, axis * 0.009f, forward * size * 0.9f, m, 8, 4);
            b.tube(c - forward * size * 0.6f, c - forward * (size + 0.025f) - axis * 0.01f, 0.005f, 0.003f, 0, m, 5, 1);
        }
    }
    // Ribs: ten pairs from the spine around the chest, sloping down to the front, with the sternum.
    {
        const auto &m = hurt(Sprite::torso);
        std::vector<V> sternum;
        for (int k = 0; k < 10; ++k) {
            const float t = 0.50f + 0.30f * k / 9.0f;
            const V spine_point = along(pose.spine, t);
            const float wide = 0.10f + 0.045f * std::sin(3.1415926f * (k + 2) / 12.0f), deep = 0.085f;
            const V centre = spine_point + forward * deep;
            // Broken ribs: the fourth to sixth snap at the side, a gap in each.
            const bool snapped = level_of(Sprite::torso) >= 2 && k >= 3 && k <= 5;
            for (const float s : {-1.0f, 1.0f}) {
                std::vector<V> rib, rest;
                for (int j = 0; j <= 9; ++j) {
                    const float theta = 3.1415926f * 0.88f * j / 9.0f;
                    const V point = centre + across * (s * wide * std::sin(theta)) - forward * (deep * std::cos(theta)) - up * (0.05f * j / 9.0f);
                    if (snapped && j > 4) rest.push_back(point + up * 0.006f);
                    else if (!(snapped && j == 4)) rib.push_back(point);
                }
                b.path(rib, 0.0055f, m);
                b.path(rest, 0.0055f, m);
            }
            if (k >= 2 && k <= 8) sternum.push_back(centre + forward * deep - up * 0.05f);
        }
        b.path(sternum, 0.011f, m);
        // Collarbones and shoulder blades.
        for (std::size_t s = 0; s < 2; ++s) {
            long_bone(b, pose.clavicle[s], pose.shoulder[s], 0.008f, 0.009f, 0.004f, level_of(Sprite::torso), 8, 4);
            const V blade = pose.shoulder[s] - forward * 0.06f + (pose.spine[pose.spine.size() / 2] - pose.shoulder[s]) * 0.25f - up * 0.06f;
            b.ellipsoid(blade, across * 0.045f, up * 0.07f, forward * 0.008f, m, 8, 6);
        }
        // Pelvis: the two wings, the sacrum behind and the pubic arch in front.
        const V mid = (pose.hip[0] + pose.hip[1]) * 0.5f;
        for (std::size_t s = 0; s < 2; ++s) {
            const V out = unit(pose.hip[s] - mid, across);
            b.ellipsoid(mid + out * 0.07f + up * 0.06f - forward * 0.01f, out * 0.07f, up * 0.06f, forward * 0.025f, m, 12, 8);
        }
        b.ellipsoid(mid - forward * 0.06f + up * 0.03f, across * 0.04f, up * 0.06f, forward * 0.015f, m, 8, 6);
        b.path({pose.hip[0] + forward * 0.04f - up * 0.03f, mid + forward * 0.07f - up * 0.05f, pose.hip[1] + forward * 0.04f - up * 0.03f},
               0.012f, m);
    }
    // Limbs: femur; tibia with the fibula beside it; humerus; radius and ulna; hands and feet.
    for (std::size_t s = 0; s < 2; ++s) {
        const V out = unit(pose.hip[s] - (pose.hip[0] + pose.hip[1]) * 0.5f, across);
        long_bone(b, pose.hip[s], pose.knee[s], 0.014f, 0.013f, 0.016f, level_of(side_sprite(s, Sprite::femur_r, Sprite::femur_l)));
        const int shin = level_of(side_sprite(s, Sprite::shin_r, Sprite::shin_l));
        long_bone(b, pose.knee[s], pose.ankle[s], 0.013f, 0.010f, 0.012f, shin);
        long_bone(b, pose.knee[s] + out * 0.028f - forward * 0.01f, pose.ankle[s] + out * 0.025f, 0.006f, 0.006f, 0.004f, shin, 6, 4);
        long_bone(b, pose.shoulder[s], pose.elbow[s], 0.012f, 0.010f, 0.011f, level_of(side_sprite(s, Sprite::humerus_r, Sprite::humerus_l)));
        const int arm = level_of(side_sprite(s, Sprite::forearm_r, Sprite::forearm_l));
        const V forearm = pose.wrist[s] - pose.elbow[s];
        V p, q;
        frame(unit(forearm, up), p, q);
        long_bone(b, pose.elbow[s] + p * 0.01f, pose.wrist[s] + p * 0.012f, 0.007f, 0.008f, 0.004f, arm, 6, 6);
        long_bone(b, pose.elbow[s] - p * 0.01f, pose.wrist[s] - p * 0.008f, 0.008f, 0.006f, 0.004f, arm, 6, 6);
        // Hand: four metacarpals fanning out to the knuckles, then the fingers.
        const auto &hand = hurt(side_sprite(s, Sprite::hand_r, Sprite::hand_l));
        const V palm = pose.fingers[s] - pose.wrist[s];
        V hp, hq;
        frame(unit(palm, up), hp, hq);
        for (int f = 0; f < 4; ++f) {
            const float spread = -0.022f + 0.0147f * f;
            const V knuckle = pose.fingers[s] + hp * (spread * 1.2f);
            b.tube(pose.wrist[s] + palm * 0.15f + hp * spread * 0.6f, knuckle, 0.004f, 0.0045f, 0.002f, hand, 5, 2);
            b.tube(knuckle, knuckle + palm * 0.55f + hp * (spread * 0.3f), 0.0035f, 0.003f, 0.001f, hand, 5, 2);
        }
        b.tube(pose.wrist[s] + hq * 0.015f, pose.wrist[s] + palm * 0.45f + hq * 0.035f - hp * 0.03f, 0.005f, 0.004f, 0.002f, hand, 5, 2); // thumb
        // Foot: the heel bone behind the ankle and five metatarsals to the toes.
        const auto &foot = hurt(side_sprite(s, Sprite::foot_r, Sprite::foot_l));
        const V length_of_foot = pose.toe[s] - pose.ankle[s];
        V fp, fq;
        frame(unit(length_of_foot, forward), fp, fq);
        b.ellipsoid(pose.ankle[s] - unit(length_of_foot) * 0.035f - up * 0.03f, fp * 0.022f, fq * 0.022f, unit(length_of_foot) * 0.035f, foot, 8, 6);
        for (int t = 0; t < 5; ++t) {
            const float spread = -0.024f + 0.012f * t;
            b.tube(pose.ankle[s] + fp * (spread * 0.5f) - up * 0.02f, pose.toe[s] + fp * spread, 0.005f, 0.004f, 0.002f, foot, 5, 2);
        }
    }
    b.draw(draw);
}
} // namespace dingosdk::overlay::detail
