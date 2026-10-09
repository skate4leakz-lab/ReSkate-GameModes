#include "bone_cam_3d.h"
#include "Extension/Modes/bone_meshes.h"
#include "Extension/Modes/bone_sprites.h"
#include <algorithm>
#include <cmath>
#include <vector>

// The X-ray's bones are a real skeleton: 215 bones from BodyParts3D (Extension/Modes/bone_meshes.h,
// baked by Extension/Modes/Tools/bake_bone_meshes.py), each riding a frame that follows the
// skater. The pelvis, the chest (ribs, sternum, collarbones, shoulder blades) and the skull ride
// the hips, the chest and the head; every vertebra rides its own place along the skater's spine,
// so the spine bends as theirs does; each limb's bones stretch from one of the skater's joints to
// the next and keep their thickness. Every triangle is projected with the game's camera, lit from
// just above the camera with a bright rim at its silhouette (an X-ray is densest where it looks
// through the edge of a bone), and drawn far to near, inside a faint shell of flesh. A cracked
// region tints orange with a dark fracture line across its bones; a broken one tints red and its
// long bones snap in two: a gap, the far half knocked out of line, splinters off both ends.

namespace dingosdk::overlay::detail {
namespace {
using V = std::array<float, 3>;
namespace mesh = modes::bone_mesh;
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

struct Material {
    float r, g, b;  // 0..1
    float alpha;    // bone: opaque-ish; flesh: scaled by the rim
    bool flesh{};
};
constexpr Material bone{0.80f, 0.90f, 1.00f, 0.95f}, cracked{1.00f, 0.66f, 0.32f, 0.95f}, broken{1.00f, 0.32f, 0.26f, 0.97f},
                   cartilage{0.55f, 0.72f, 0.95f, 0.45f}, fracture{0.03f, 0.05f, 0.12f, 0.97f}, flesh{0.35f, 0.65f, 0.95f, 1.0f, true};

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
    // A tube from a to b, radius r0 easing to r1 (the flesh's limbs, splinters).
    void tube(const V &a, const V &b, float r0, float r1, const Material &m, int sides = 10, int rings = 3) {
        const V axis = b - a;
        if (length(axis) < 0.003f) return;
        const V u = unit(axis), helper = std::abs(u[1]) < 0.9f ? V{0, 1, 0} : V{1, 0, 0};
        const V p = unit(cross(u, helper)), q = cross(u, p);
        const auto at = [&](int ring, int side) {
            const float t = static_cast<float>(ring) / rings, angle = 6.2831853f * side / sides;
            return a + axis * t + (p * std::cos(angle) + q * std::sin(angle)) * (r0 + (r1 - r0) * t);
        };
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < sides; ++j) quad(at(i, j), at(i, j + 1), at(i + 1, j + 1), at(i + 1, j), m);
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
        // Thousands of small, overlapping triangles over a dimmed screen: their edges need no
        // smoothing, which would add a fringe of vertices to every one.
        const auto flags = list->Flags;
        list->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (const auto &t : tris)
            if ((t.colour >> IM_COL32_A_SHIFT) & 0xff) list->AddTriangleFilled(t.p[0], t.p[1], t.p[2], t.colour);
        list->Flags = flags;
        tris.clear();
    }
};

// A point along a polyline, `t` of its length from its start, and its direction there.
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
V tangent(const std::vector<V> &line, float t, float reach) {
    return unit(along(line, std::min(1.0f, t + reach)) - along(line, std::max(0.0f, t - reach)));
}

// A frame as the baker built it: a primary axis kept, the secondary and tertiary made square to it.
struct Frame {
    V origin, p, s, t;
    float limb{}; // a limb frame's length now (metres), else 0
};
Frame make_frame(const V &origin, const V &primary, const V &secondary, const V &tertiary, float limb = 0) {
    Frame f{origin, unit(primary), {}, {}, limb};
    f.s = unit(secondary - f.p * dot(secondary, f.p), {1, 0, 0});
    f.t = unit(tertiary - f.p * dot(tertiary, f.p) - f.s * dot(tertiary, f.s), {0, 0, 1});
    return f;
}
} // namespace

void draw_xray_skeleton(ImDrawList *draw, const BoneCamPose &pose, const XrayView &view, float fade) {
    if (!pose.valid || pose.spine.size() < 3 || fade <= 0) return;
    const auto level_of = [&](std::uint8_t region) { return region < pose.hurt.size() ? static_cast<int>(pose.hurt[region]) : 0; };

    // The body's directions: up the spine, across the shoulders (to the skater's left), and back
    // (opposite the way the toes point, which a ragdoll keeps roughly in front of its hips).
    const V hips = pose.spine.front(), neck = pose.spine[pose.spine.size() - 2];
    const V up = unit(neck - hips);
    const V across = unit(pose.shoulder[1] - pose.shoulder[0], view.right);
    V forward = unit(cross(across, up), view.back * -1.0f);
    const V toes = (pose.toe[0] - pose.ankle[0]) + (pose.toe[1] - pose.ankle[1]);
    if (dot(forward, toes) < 0) forward = forward * -1.0f;
    const V back = forward * -1.0f;

    // The skater's frames, in the baker's order (bone_meshes.h). Sides: pose [0] right, [1] left.
    std::array<Frame, mesh::frame_count> frames{};
    const V hip_mid = (pose.hip[0] + pose.hip[1]) * 0.5f;
    frames[0] = make_frame(hip_mid, tangent(pose.spine, 0.08f, 0.08f), pose.hip[1] - pose.hip[0], back);
    const float chest_t = mesh::frames[1].spine;
    frames[1] = make_frame(along(pose.spine, chest_t), tangent(pose.spine, chest_t, 0.12f), pose.shoulder[1] - pose.shoulder[0], back);
    {
        const V head_up = unit(pose.head_up, up);
        const V face = unit(forward - head_up * dot(forward, head_up), forward);
        frames[2] = make_frame(pose.head, head_up, face * -1.0f, across);
    }
    const std::array<std::pair<const std::array<V, 2> *, const std::array<V, 2> *>, 6> limbs{{
        {&pose.shoulder, &pose.elbow}, {&pose.elbow, &pose.wrist}, {&pose.wrist, &pose.fingers},
        {&pose.hip, &pose.knee}, {&pose.knee, &pose.ankle}, {&pose.ankle, &pose.toe}}};
    float ratio_sum = 0;
    int ratios = 0;
    for (std::size_t l = 0; l < limbs.size(); ++l)
        for (std::size_t side = 0; side < 2; ++side) {
            const auto index = 3 + l * 2 + side;
            const V a = (*limbs[l].first)[side], b = (*limbs[l].second)[side];
            const float now = length(b - a);
            // The foot is square to the shin; every other limb to the body's back.
            const V secondary = l == 5 ? pose.knee[side] - a : back;
            frames[index] = make_frame(a, b - a, secondary, across, now);
            if (l != 2 && l != 5 && mesh::frames[index].limb > 0.05f) { // arms and legs: the steadiest lengths
                ratio_sum += now / mesh::frames[index].limb;
                ++ratios;
            }
        }
    // How much bigger or smaller than the model this skater is: bones keep that thickness.
    const float scale = std::clamp(ratios ? ratio_sum / static_cast<float>(ratios) : 1.0f, 0.75f, 1.5f);
    for (std::size_t f = mesh::vertebra_frame; f < mesh::frame_count; ++f) {
        const float t = mesh::frames[f].spine;
        frames[f] = make_frame(along(pose.spine, t), tangent(pose.spine, t, 0.05f), back, across);
    }

    // Flesh first: everything inside it is drawn over it.
    Builder skin(view, fade);
    {
        const V torso_centre = along(pose.spine, 0.42f) + forward * 0.03f;
        skin.ellipsoid(torso_centre, across * 0.19f, up * (length(neck - hips) * 0.62f), forward * 0.13f, flesh, 16, 10);
        skin.ellipsoid(pose.head + unit(pose.head_up, up) * 0.09f, across * 0.11f, unit(pose.head_up, up) * 0.14f, forward * 0.12f, flesh, 14, 9);
        for (std::size_t s = 0; s < 2; ++s) {
            skin.tube(pose.hip[s], pose.knee[s], 0.085f, 0.06f, flesh, 12);
            skin.tube(pose.knee[s], pose.ankle[s], 0.06f, 0.04f, flesh, 12);
            skin.tube(pose.shoulder[s], pose.elbow[s], 0.055f, 0.045f, flesh, 10);
            skin.tube(pose.elbow[s], pose.wrist[s], 0.045f, 0.035f, flesh, 10);
            skin.tube(pose.wrist[s], pose.fingers[s] + (pose.fingers[s] - pose.wrist[s]) * 0.6f, 0.035f, 0.025f, flesh, 8, 2);
            skin.tube(pose.ankle[s] - forward * 0.04f, pose.toe[s] + (pose.toe[s] - pose.ankle[s]) * 0.25f, 0.045f, 0.035f, flesh, 8, 2);
        }
    }
    skin.draw(draw);

    // The bones.
    Builder b(view, fade);
    std::vector<V> world;
    for (const auto &m : mesh::meshes) {
        const auto &f = frames[m.frame];
        const int level = level_of(m.region);
        const bool snapped = level >= 2 && (m.flags & mesh::snaps);
        const auto &material = (m.flags & mesh::cartilage) ? cartilage : level >= 2 ? broken : level == 1 ? cracked : bone;
        // A snapped bone's far half (past the middle of its length): knocked sideways and bent.
        const float kick = 0.016f * scale;
        world.resize(m.vertex_count);
        for (std::uint32_t i = 0; i < m.vertex_count; ++i) {
            const auto &v = mesh::vertices[m.first_vertex + i];
            V local_s = f.s * (v[1] * scale), local_t = f.t * (v[2] * scale);
            V p = f.limb > 0 ? f.origin + f.p * (v[0] * f.limb) + local_s + local_t : f.origin + (f.p * v[0] + f.s * v[1] + f.t * v[2]) * scale;
            if (snapped) {
                const int a = mesh::along[m.first_vertex + i];
                if (a > 128) {
                    const float out = static_cast<float>(a - 128) / 127.0f;
                    p = p + f.s * (kick * (0.6f + 0.6f * out)) + f.t * (kick * 0.4f) + f.p * (0.006f * scale);
                }
            }
            world[i] = p;
        }
        V break_sum{};
        int break_count = 0;
        for (std::uint32_t k = 0; k + 2 < m.index_count; k += 3) {
            const auto i0 = mesh::indices[m.first_index + k], i1 = mesh::indices[m.first_index + k + 1], i2 = mesh::indices[m.first_index + k + 2];
            const int a0 = mesh::along[m.first_vertex + i0], a1 = mesh::along[m.first_vertex + i1], a2 = mesh::along[m.first_vertex + i2];
            const int lo = std::min({a0, a1, a2}), hi = std::max({a0, a1, a2});
            if (snapped && lo <= 128 && hi > 128) { // the break itself: left open
                break_sum = break_sum + world[i0];
                ++break_count;
                continue;
            }
            // A crack: a dark line round the bone's middle; a broken skull shows several.
            bool line = false;
            if (level >= 1 && !snapped && (m.flags & (mesh::snaps | mesh::cracks))) {
                const auto across_line = [&](int at, int width) { return lo <= at + width && hi >= at - width; };
                line = across_line(128, 3) || (level >= 2 && (m.flags & mesh::cracks) && (across_line(70, 3) || across_line(190, 3)));
            }
            b.tri(world[i0], world[i1], world[i2], line ? fracture : material);
        }
        // Splinters standing off both broken ends.
        if (snapped && break_count > 0 && f.limb > 0) {
            const V at = break_sum * (1.0f / static_cast<float>(break_count));
            for (int i = 0; i < 4; ++i) {
                const float angle = 1.57f * i + 0.4f;
                const V out = f.s * std::cos(angle) + f.t * std::sin(angle);
                const float tall = (0.008f + 0.004f * (i % 2)) * scale;
                b.tube(at + out * (0.004f * scale), at - f.p * tall + out * (0.008f * scale), 0.003f * scale, 0.0006f, broken, 4, 1);
                const V far = at + f.s * kick + f.t * (kick * 0.4f);
                b.tube(far + out * (0.004f * scale), far + f.p * tall + out * (0.008f * scale), 0.003f * scale, 0.0006f, broken, 4, 1);
            }
        }
    }
    b.draw(draw);
}
} // namespace dingosdk::overlay::detail
