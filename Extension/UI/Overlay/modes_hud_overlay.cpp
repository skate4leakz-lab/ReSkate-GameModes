#include "overlay_internal.h"
#include "Extension/UI/skate_theme.h"
#include "Extension/HallOfMeat/hall_of_meat_overlay.h"
#include <cmath>
#include <format>
#include <optional>
#include <span>

// ReSkate's game modes HUD (Extension/Modes/game_modes.h). Over the world: the area the
// leader marked out as a fence, checkpoints and spots as rings (the next checkpoint with a
// beam, spots in their holder's colour) and Graffiti's zones painted in their taggers'
// colours. On screen: the scoreboard on the right, the clock, the latest callout at the top,
// the countdown in the middle and the local player's line at the bottom.
// Background draw list, under ReSkate's own menus and chat; it takes no input.

namespace dingosdk::overlay {
namespace {
std::atomic<ModesHudFeed> modes_hud_feed{};
}
void set_modes_hud_feed(ModesHudFeed feed) noexcept { modes_hud_feed.store(feed); }
} // namespace dingosdk::overlay

using namespace dingosdk::overlay::detail;
namespace dingosdk::overlay::detail {
namespace {
namespace theme = dingosdk::skate_theme;
using Clock = std::chrono::steady_clock;
using Vec3 = std::array<float, 3>;

struct HudState {
    ModesHud hud;
    std::uint64_t banner_serial{};
    Clock::time_point banner_at{};
};
HudState &hud_state() {
    static HudState value;
    return value;
}
ImU32 with_alpha(ImU32 colour, float alpha) {
    const auto a = static_cast<unsigned>(((colour >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}
float seconds_since(Clock::time_point at) { return std::chrono::duration<float>(Clock::now() - at).count(); }
// The face to draw `text` with at `size`: the large bake of the title or heading face once the text is
// drawn well past their menu sizes (a stretched small bake shows its pixels), when the text is
// plain ASCII (all the large bakes hold).
ImFont *crisp(ImFont *font, float size, const std::string &text) {
    const auto &menu = state().menu;
    if (std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) return font;
    if (font == menu.title && menu.title_large && size > 52.0f) return menu.title_large;
    if ((font == menu.heading || font == menu.bold) && menu.heading_large && size > 27.0f) return menu.heading_large;
    return font;
}
void shadowed(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text) {
    font = crisp(font, size, text);
    const float offset = std::max(1.0f, size / 16.0f);
    const auto alpha = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f;
    draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), with_alpha(IM_COL32(0, 0, 0, 255), alpha * 0.7f),
                  text.c_str());
    draw->AddText(font, size, at, colour, text.c_str());
}
void centred(ImDrawList *draw, ImFont *font, float size, float x, float y, ImU32 colour, const std::string &text) {
    const auto extent = crisp(font, size, text)->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    shadowed(draw, font, size, ImVec2(x - extent.x * 0.5f, y), colour, text);
}

// The camera, as the nametags place things: points in its space, then on screen.
struct Camera {
    Vec3 right{}, up{}, back{}, origin{};
    float focal{};
    ImVec2 centre{};
    struct Point {
        float side{}, height{}, depth{};
    };
    Point view(const Vec3 &p) const {
        const Vec3 d{p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
        const auto dot = [&](const Vec3 &a) { return d[0] * a[0] + d[1] * a[1] + d[2] * a[2]; };
        return {dot(right), dot(up), -dot(back)};
    }
    ImVec2 screen(const Point &p) const {
        return ImVec2(centre.x + p.side * focal / p.depth, centre.y - p.height * focal / p.depth);
    }
    std::optional<ImVec2> project(const Vec3 &p) const {
        const auto v = view(p);
        if (v.depth <= near_plane) return std::nullopt;
        return screen(v);
    }
    // A segment cut at the near_plane plane: nothing when it is wholly behind the camera.
    bool segment(const Vec3 &a, const Vec3 &b, ImVec2 &sa, ImVec2 &sb) const {
        auto va = view(a), vb = view(b);
        if (va.depth <= near_plane && vb.depth <= near_plane) return false;
        const auto cut = [&](Point &behind, const Point &front) {
            const float t = (near_plane - behind.depth) / (front.depth - behind.depth);
            behind = {behind.side + (front.side - behind.side) * t, behind.height + (front.height - behind.height) * t, near_plane};
        };
        if (va.depth <= near_plane) cut(va, vb);
        else if (vb.depth <= near_plane) cut(vb, va);
        sa = screen(va);
        sb = screen(vb);
        return true;
    }
    float distance(const Vec3 &p) const {
        const float dx = p[0] - origin[0], dy = p[1] - origin[1], dz = p[2] - origin[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    static constexpr float near_plane = 0.2f;
};

void line3(ImDrawList *draw, const Camera &cam, const Vec3 &a, const Vec3 &b, ImU32 colour, float thickness) {
    ImVec2 sa, sb;
    if (cam.segment(a, b, sa, sb)) draw->AddLine(sa, sb, colour, thickness);
}
void ring(ImDrawList *draw, const Camera &cam, const Vec3 &centre, float radius, ImU32 colour, float thickness) {
    constexpr int steps = 28;
    for (int i = 0; i < steps; ++i) {
        const float a0 = 6.2831853f * i / steps, a1 = 6.2831853f * (i + 1) / steps;
        line3(draw, cam, {centre[0] + std::cos(a0) * radius, centre[1] + 0.15f, centre[2] + std::sin(a0) * radius},
              {centre[0] + std::cos(a1) * radius, centre[1] + 0.15f, centre[2] + std::sin(a1) * radius}, colour, thickness);
    }
}
void filled_ring(ImDrawList *draw, const Camera &cam, const Vec3 &centre, float radius, ImU32 colour) {
    constexpr int steps = 28;
    const auto middle = cam.project({centre[0], centre[1] + 0.12f, centre[2]});
    if (!middle) return;
    for (int i = 0; i < steps; ++i) {
        const float a0 = 6.2831853f * i / steps, a1 = 6.2831853f * (i + 1) / steps;
        const auto p0 = cam.project({centre[0] + std::cos(a0) * radius, centre[1] + 0.12f, centre[2] + std::sin(a0) * radius});
        const auto p1 = cam.project({centre[0] + std::cos(a1) * radius, centre[1] + 0.12f, centre[2] + std::sin(a1) * radius});
        if (p0 && p1) draw->AddTriangleFilled(*middle, *p0, *p1, colour);
    }
}

constexpr ImU32 placing_colour = IM_COL32(64, 170, 255, 255);

// A flat polygon in the world with a colour at each corner, cut where it passes behind the camera.
void fill_world(ImDrawList *draw, const Camera &cam, std::span<const Vec3> points, std::span<const ImU32> colours) {
    struct Corner {
        Camera::Point at;
        ImVec4 colour;
    };
    std::vector<Corner> in, out;
    for (std::size_t i = 0; i < points.size(); ++i) in.push_back({cam.view(points[i]), ImGui::ColorConvertU32ToFloat4(colours[i])});
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto &a = in[i], &b = in[(i + 1) % in.size()];
        const bool a_in = a.at.depth > Camera::near_plane, b_in = b.at.depth > Camera::near_plane;
        if (a_in) out.push_back(a);
        if (a_in != b_in) {
            const float t = (Camera::near_plane - a.at.depth) / (b.at.depth - a.at.depth);
            const auto mix = [t](float x, float y) { return x + (y - x) * t; };
            out.push_back({{mix(a.at.side, b.at.side), mix(a.at.height, b.at.height), Camera::near_plane},
                           ImVec4(mix(a.colour.x, b.colour.x), mix(a.colour.y, b.colour.y), mix(a.colour.z, b.colour.z),
                                  mix(a.colour.w, b.colour.w))});
        }
    }
    if (out.size() < 3) return;
    const auto uv = ImGui::GetFontTexUvWhitePixel();
    draw->PrimReserve(static_cast<int>((out.size() - 2) * 3), static_cast<int>(out.size()));
    // After the reserve: past 64k vertices it starts a new vertex offset and the index restarts at 0.
    const auto first = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
    for (const auto &c : out) draw->PrimWriteVtx(cam.screen(c.at), uv, ImGui::ColorConvertFloat4ToU32(c.colour));
    for (std::size_t k = 1; k + 1 < out.size(); ++k) {
        draw->PrimWriteIdx(first);
        draw->PrimWriteIdx(static_cast<ImDrawIdx>(first + k));
        draw->PrimWriteIdx(static_cast<ImDrawIdx>(first + k + 1));
    }
}
float horizontal_to_segment(const Vec3 &p, const Vec3 &a, const Vec3 &b) {
    const float dx = b[0] - a[0], dz = b[2] - a[2], length = dx * dx + dz * dz;
    const float t = length > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[2] - a[2]) * dz) / length, 0.0f, 1.0f) : 0.0f;
    const float x = a[0] + dx * t - p[0], z = a[2] + dz * t - p[2];
    return std::sqrt(x * x + z * z);
}

// The play area's edge, the way skate.'s throwdowns mark theirs: a tall see-through wall and
// nothing else (no line on the ground, no posts). It is an even veil from the ground up, fading
// out near its top, with faint ribs and slow soft swells drifting up it; seen from anywhere, a
// little clearer where the skater comes close. While being placed it is blue, with its foot
// marked so the leader sees exactly where it stands.
void draw_boundary(ImDrawList *draw, const Camera &cam, const ModesHud &h, float scale) {
    std::vector<std::pair<Vec3, Vec3>> edges;
    if (h.area_radius > 0 && !h.corners.empty()) {
        const auto &c = h.corners.front();
        const int segments = std::clamp(static_cast<int>(h.area_radius * 2.0f), 32, 128);
        for (int i = 0; i < segments; ++i) {
            const float a0 = 6.2831853f * i / segments, a1 = 6.2831853f * (i + 1) / segments;
            edges.push_back({{c[0] + std::cos(a0) * h.area_radius, c[1], c[2] + std::sin(a0) * h.area_radius},
                             {c[0] + std::cos(a1) * h.area_radius, c[1], c[2] + std::sin(a1) * h.area_radius}});
        }
    } else if (h.corners.size() >= 2) {
        const bool closed = h.corners.size() >= 3;
        for (std::size_t i = 0; i + (closed ? 0 : 1) < h.corners.size(); ++i)
            edges.push_back({h.corners[i], h.corners[(i + 1) % h.corners.size()]});
    }
    if (edges.empty()) return;
    const ImU32 base = h.placing ? placing_colour : theme::bar;
    const float time = static_cast<float>(ImGui::GetTime());
    // As tall as a throwdown's wall, and taller when the skater climbs: the area has no lid.
    const float floor_y = edges.front().first[1];
    const float height = std::max(25.0f, h.have_me ? h.me[1] - floor_y + 12.0f : 25.0f), solid = height * 0.7f;
    const auto at = [](const Vec3 &p, float up) { return Vec3{p[0], p[1] + up, p[2]}; };
    for (const auto &[a, b] : edges) {
        if (cam.distance(a) > 600.0f && cam.distance(b) > 600.0f) continue;
        // Drawn over the world (through buildings too), so the wall shows only where it matters, as a
        // throwdown's does: near the skater, fading in from 25 m. While it is placed, a low, faint
        // wall over the line on the ground instead of a full height one everywhere.
        if (h.placing) {
            if (cam.distance(a) > 300.0f && cam.distance(b) > 300.0f) continue;
            const float low = 2.5f;
            const Vec3 wall[]{at(a, 0.0f), at(b, 0.0f), at(b, low), at(a, low)};
            const ImU32 foot = with_alpha(base, 0.16f), top = with_alpha(base, 0.0f);
            const ImU32 shade[]{foot, foot, top, top};
            fill_world(draw, cam, wall, shade);
            line3(draw, cam, at(a, 0.03f), at(b, 0.03f), with_alpha(theme::white, 0.8f), std::max(1.2f, 2.0f * scale));
            continue;
        }
        const float gap = h.have_me ? horizontal_to_segment(h.me, a, b) : 60.0f;
        const float closeness = std::clamp(1.0f - gap / 25.0f, 0.0f, 1.0f);
        if (closeness <= 0.0f) continue; // far from this edge: nothing of it shows
        const float veil = 0.22f * closeness;
        // The veil: even from the ground to most of the way up, then fading out.
        {
            const Vec3 lower[]{at(a, 0.0f), at(b, 0.0f), at(b, solid), at(a, solid)};
            const Vec3 upper[]{at(a, solid), at(b, solid), at(b, height), at(a, height)};
            const ImU32 even = with_alpha(base, veil), none = with_alpha(base, 0.0f);
            const ImU32 lower_shade[]{even, even, even, even}, upper_shade[]{even, even, none, none};
            fill_world(draw, cam, lower, lower_shade);
            fill_world(draw, cam, upper, upper_shade);
        }
        const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2], length = std::sqrt(dx * dx + dz * dz);
        if (length > 0.05f) {
            const auto along = [&](float s, float up) {
                const float t = std::clamp(s / length, 0.0f, 1.0f);
                return Vec3{a[0] + dx * t, a[1] + dy * t + up, a[2] + dz * t};
            };
            // Faint ribs every 2 m, a texture more than lines; only near the camera and in front of it.
            for (float s = 0; s <= length; s += 2.0f) {
                const Vec3 foot = along(s, 0.0f), head = along(s, solid);
                if (cam.distance(foot) > 120.0f ||
                    (cam.view(foot).depth <= Camera::near_plane && cam.view(head).depth <= Camera::near_plane))
                    continue;
                const Vec3 quad[]{foot, along(s + 0.08f, 0.0f), along(s + 0.08f, solid), head};
                const ImU32 faint = with_alpha(theme::white, 0.075f * closeness), none = with_alpha(theme::white, 0.0f);
                const ImU32 shades[]{faint, faint, none, none};
                fill_world(draw, cam, quad, shades);
            }
            // Slow, wide swells drifting up the veil: no edges, just a breath of light.
            for (int swell = 0; swell < 2; ++swell) {
                const float y = std::fmod(time * 1.0f + swell * 9.0f, 18.0f), thick = 3.0f, fade = 1.0f - y / 18.0f;
                const ImU32 glow = with_alpha(base, 0.11f * closeness * fade), none = with_alpha(base, 0.0f);
                const Vec3 lower[]{along(0, y), along(length, y), along(length, y + thick), along(0, y + thick)};
                const Vec3 upper[]{along(0, y + thick), along(length, y + thick), along(length, y + thick * 2), along(0, y + thick * 2)};
                const ImU32 rise[]{none, none, glow, glow}, fall[]{glow, glow, none, none};
                fill_world(draw, cam, lower, rise);
                fill_world(draw, cam, upper, fall);
            }
            // Where the skater is close, the veil brightens softly round them at their height.
            if (h.have_me && closeness > 0.3f) {
                const float ax = h.me[0] - a[0], az = h.me[2] - a[2];
                const float s = std::clamp((ax * dx + az * dz) / length, 0.0f, length), y = std::max(0.0f, h.me[1] - a[1]);
                const float w = 4.0f, tall = 3.0f, glow = closeness * closeness;
                const ImU32 hot = with_alpha(theme::white, 0.16f * glow), edge = with_alpha(base, 0.0f);
                const Vec3 left[]{along(s - w, y - tall + 1), along(s, y - tall + 1), along(s, y + tall + 1), along(s - w, y + tall + 1)};
                const Vec3 right[]{along(s, y - tall + 1), along(s + w, y - tall + 1), along(s + w, y + tall + 1), along(s, y + tall + 1)};
                const ImU32 l_shade[]{edge, hot, hot, edge}, r_shade[]{hot, edge, edge, hot};
                fill_world(draw, cam, left, l_shade);
                fill_world(draw, cam, right, r_shade);
            }
        }
    }
}
// A neon glow along a path in the world: a wide soft halo, a coloured body and a hot white core,
// each sized in metres so it hugs a rail up close and stays a thin line far away.
void glow_path(ImDrawList *draw, const Camera &cam, const std::vector<Vec3> &path, ImU32 colour, float metres, float strength) {
    if (path.size() < 2) return;
    std::vector<ImVec2> points;
    float depth = 0;
    for (const auto &p : path) {
        const auto v = cam.view(p);
        if (v.depth <= Camera::near_plane) return; // partly behind the camera: skipped this frame
        points.push_back(cam.screen(v));
        depth += v.depth;
    }
    depth /= static_cast<float>(path.size());
    const float width = std::clamp(metres * cam.focal / std::max(depth, 0.5f), 1.5f, 70.0f);
    const auto light = [&](float t) {
        const auto c = ImGui::ColorConvertU32ToFloat4(colour);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x + (1 - c.x) * t, c.y + (1 - c.y) * t, c.z + (1 - c.z) * t, 1));
    };
    struct Layer {
        float width, alpha, white;
    };
    constexpr Layer layers[]{{3.4f, 0.08f, 0.0f}, {2.2f, 0.16f, 0.0f}, {1.3f, 0.45f, 0.1f}, {0.6f, 0.9f, 0.45f}, {0.2f, 0.85f, 0.9f}};
    for (const auto &layer : layers)
        draw->AddPolyline(points.data(), static_cast<int>(points.size()), with_alpha(light(layer.white), layer.alpha * strength),
                          ImDrawFlags_RoundCornersAll, std::max(1.0f, width * layer.width));
}

// A Graffiti tag, the way THPS lights up what you skated. A grind glows along the exact path the
// board slid on the curb, ledge or rail; a gap glows where it landed, its arc traced from the takeoff.
void draw_tag(ImDrawList *draw, const Camera &cam, const ModesHudTag &tag) {
    if (tag.path.size() < 2 || cam.distance(tag.path.front()) > 250.0f) return;
    const float pulse = 0.85f + 0.15f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
    if (!tag.gap) {
        std::vector<Vec3> lifted;
        for (const auto &p : tag.path) lifted.push_back({p[0], p[1] + 0.03f, p[2]});
        glow_path(draw, cam, lifted, tag.color, 0.12f, pulse);
        return;
    }
    const auto &from = tag.path.front(), &to = tag.path.back();
    std::vector<Vec3> arc, landing;
    constexpr int steps = 20;
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / steps;
        arc.push_back({from[0] + (to[0] - from[0]) * t, from[1] + (to[1] - from[1]) * t + 3.2f * t * (1 - t) + 0.3f,
                       from[2] + (to[2] - from[2]) * t});
    }
    constexpr int around = 28;
    for (int i = 0; i <= around; ++i) {
        const float a = 6.2831853f * i / around;
        landing.push_back({to[0] + std::cos(a) * 1.2f, to[1] + 0.05f, to[2] + std::sin(a) * 1.2f});
    }
    glow_path(draw, cam, arc, tag.color, 0.07f, pulse * 0.8f);
    glow_path(draw, cam, landing, tag.color, 0.12f, pulse);
}

// ---- skate.'s look -----------------------------------------------------------------------
// The game's own HUD has no boxes: white text with a soft shadow straight on the picture, on
// dark bands that fade out (the location plate, the toasts), orange diamond markers and an orange
// accent. Everything below draws in that style.
constexpr ImU32 accent = IM_COL32(255, 168, 0, 255), start_blue = IM_COL32(1, 131, 255, 255);

void soft_text(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text) {
    font = crisp(font, size, text);
    const auto alpha = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f;
    const float blur = std::max(1.0f, size / 14.0f);
    for (const auto &[dx, dy] : {std::pair{-blur, 0.0f}, {blur, 0.0f}, {0.0f, -blur}, {0.0f, blur}})
        draw->AddText(font, size, ImVec2(at.x + dx, at.y + dy + blur * 0.6f), with_alpha(IM_COL32(0, 0, 0, 255), alpha * 0.22f), text.c_str());
    draw->AddText(font, size, ImVec2(at.x, at.y + blur * 0.8f), with_alpha(IM_COL32(0, 0, 0, 255), alpha * 0.55f), text.c_str());
    draw->AddText(font, size, at, colour, text.c_str());
}
float text_width(ImFont *font, float size, const std::string &text) {
    return crisp(font, size, text)->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
}
// A dark band fading out to one side, or both (the game's location plate and toasts).
void band(ImDrawList *draw, ImVec2 a, ImVec2 b, float alpha, int fade) { // fade: -1 to the left, 1 to the right, 0 both ways
    const ImU32 dark = IM_COL32(8, 10, 14, static_cast<int>(255 * alpha)), clear = IM_COL32(8, 10, 14, 0);
    if (fade == 0) {
        const float mid = (a.x + b.x) * 0.5f;
        draw->AddRectFilledMultiColor(a, ImVec2(mid, b.y), clear, dark, dark, clear);
        draw->AddRectFilledMultiColor(ImVec2(mid, a.y), b, dark, clear, clear, dark);
    } else if (fade < 0) {
        draw->AddRectFilledMultiColor(a, b, clear, dark, dark, clear);
    } else {
        draw->AddRectFilledMultiColor(a, b, dark, clear, clear, dark);
    }
}
// The orange diamond skate. marks its spots and events with.
void diamond(ImDrawList *draw, ImVec2 c, float r, ImU32 colour) {
    draw->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), colour);
    draw->AddQuad(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), IM_COL32(255, 255, 255, 200), std::max(1.0f, r * 0.14f));
}
// ---- neon, for the countdown and GO!: clean lines of light, a soft wide glow, a bright body and a
// hot white core, the way a neon tube looks.
constexpr ImU32 neon_blue = IM_COL32(0, 229, 255, 255), neon_go = IM_COL32(110, 255, 140, 255);
ImU32 whiten(ImU32 colour, float t) {
    const auto c = ImGui::ColorConvertU32ToFloat4(colour);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x + (1 - c.x) * t, c.y + (1 - c.y) * t, c.z + (1 - c.z) * t, c.w));
}
float alpha_of(ImU32 colour) { return static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f; }
struct NeonLayer {
    float width, alpha, white;
};
constexpr NeonLayer neon_layers[]{{7.0f, 0.05f, 0.0f}, {3.8f, 0.10f, 0.0f}, {2.0f, 0.38f, 0.05f}, {1.0f, 0.95f, 0.30f}, {0.45f, 0.95f, 0.85f}};
void neon_arc(ImDrawList *draw, ImVec2 c, float r, float from, float to, ImU32 colour, float line) {
    for (const auto &layer : neon_layers) {
        draw->PathArcTo(c, r, from, to, 96);
        draw->PathStroke(with_alpha(whiten(colour, layer.white), layer.alpha * alpha_of(colour)), ImDrawFlags_None, line * layer.width);
    }
}
void neon_dot(ImDrawList *draw, ImVec2 at, float r, ImU32 colour) {
    const float a = alpha_of(colour);
    draw->AddCircleFilled(at, r * 3.2f, with_alpha(colour, 0.06f * a), 24);
    draw->AddCircleFilled(at, r * 1.9f, with_alpha(colour, 0.20f * a), 24);
    draw->AddCircleFilled(at, r, with_alpha(whiten(colour, 0.3f), a), 20);
    draw->AddCircleFilled(at, r * 0.5f, with_alpha(IM_COL32(255, 255, 255, 255), a), 16);
}
// Text lit like a sign: rings of faint colour round it for the glow, then the letters near white.
void neon_text(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text) {
    font = crisp(font, size, text);
    const float a = alpha_of(colour);
    for (const auto &[spread, strength] : {std::pair{0.085f, 0.035f}, std::pair{0.05f, 0.06f}, std::pair{0.022f, 0.12f}})
        for (int k = 0; k < 12; ++k) {
            const float angle = 6.2831853f * static_cast<float>(k) / 12.0f;
            draw->AddText(font, size, ImVec2(at.x + std::cos(angle) * size * spread, at.y + std::sin(angle) * size * spread),
                          with_alpha(colour, strength * a), text.c_str());
        }
    draw->AddText(font, size, at, with_alpha(whiten(colour, 0.82f), a), text.c_str());
}
// A short label with its letters spread out, centred on x.
void spaced_text(ImDrawList *draw, ImFont *font, float size, float x, float y, ImU32 colour, const std::string &text, float spacing) {
    float total = 0;
    for (const char ch : text) total += text_width(font, size, std::string(1, ch)) + spacing;
    total -= spacing;
    float at = x - total * 0.5f;
    for (const char ch : text) {
        const std::string letter(1, ch);
        soft_text(draw, font, size, ImVec2(at, y), colour, letter);
        at += text_width(font, size, letter) + spacing;
    }
}

// A D-pad as the game draws its prompts, the direction to press lit.
void dpad_glyph(ImDrawList *draw, ImVec2 c, float size, char lit) {
    const float arm = size * 0.5f, thick = size * 0.34f;
    const ImU32 base = IM_COL32(255, 255, 255, 90), on = IM_COL32(255, 255, 255, 255);
    draw->AddRectFilled(ImVec2(c.x - thick * 0.5f, c.y - arm), ImVec2(c.x + thick * 0.5f, c.y + arm), base, thick * 0.25f);
    draw->AddRectFilled(ImVec2(c.x - arm, c.y - thick * 0.5f), ImVec2(c.x + arm, c.y + thick * 0.5f), base, thick * 0.25f);
    const auto light = [&](ImVec2 a, ImVec2 b) { draw->AddRectFilled(a, b, on, thick * 0.25f); };
    switch (lit) {
    case 'U': light(ImVec2(c.x - thick * 0.5f, c.y - arm), ImVec2(c.x + thick * 0.5f, c.y - thick * 0.4f)); break;
    case 'D': light(ImVec2(c.x - thick * 0.5f, c.y + thick * 0.4f), ImVec2(c.x + thick * 0.5f, c.y + arm)); break;
    case 'L': light(ImVec2(c.x - arm, c.y - thick * 0.5f), ImVec2(c.x - thick * 0.4f, c.y + thick * 0.5f)); break;
    case 'R': light(ImVec2(c.x + thick * 0.4f, c.y - thick * 0.5f), ImVec2(c.x + arm, c.y + thick * 0.5f)); break;
    default: break;
    }
}

// The scoreboard on the right: the mode and clock over a thin accent line, what to do, then the
// standings, each on a band fading in from the screen's edge.
void draw_panel(ImDrawList *draw, HudState &st, float scale) {
    auto &s = state();
    const auto &h = st.hud;
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto *body = s.menu.body ? s.menu.body : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float right = display.x - 36.0f * scale, width = 360.0f * scale, left = right - width;
    float y = 112.0f * scale;
    // Mode name and clock.
    std::string mode = h.title;
    for (auto &c : mode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    band(draw, ImVec2(left - 40.0f * scale, y - 6.0f * scale), ImVec2(display.x, y + 58.0f * scale), 0.55f, -1);
    soft_text(draw, heading, 22.0f * scale, ImVec2(left, y + 4.0f * scale), IM_COL32(255, 255, 255, 255), mode);
    if (h.clock.size() > 1) {
        const bool low = h.clock.size() >= 4 && h.clock.rfind("0:", 0) == 0 && h.clock[2] == '0';
        const float pulse = low ? 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f) : 1.0f;
        const ImU32 colour = low ? with_alpha(theme::danger, pulse) : IM_COL32(255, 255, 255, 255);
        soft_text(draw, heading, 38.0f * scale, ImVec2(right - text_width(heading, 38.0f * scale, h.clock), y - 2.0f * scale), colour, h.clock);
    }
    draw->AddRectFilledMultiColor(ImVec2(left, y + 40.0f * scale), ImVec2(right, y + 43.0f * scale), with_alpha(accent, 0.0f), accent, accent,
                                  with_alpha(accent, 0.0f));
    y += 54.0f * scale;
    if (!h.status.empty()) {
        const float wrap = width;
        const auto extent = body->CalcTextSizeA(15.0f * scale, FLT_MAX, wrap, h.status.c_str());
        band(draw, ImVec2(left - 40.0f * scale, y - 4.0f * scale), ImVec2(display.x, y + extent.y + 6.0f * scale), 0.4f, -1);
        draw->AddText(body, 15.0f * scale, ImVec2(left + 1, y + 1), IM_COL32(0, 0, 0, 160), h.status.c_str(), nullptr, wrap);
        draw->AddText(body, 15.0f * scale, ImVec2(left, y), IM_COL32(230, 232, 236, 255), h.status.c_str(), nullptr, wrap);
        y += extent.y + 14.0f * scale;
    }
    const float row = 36.0f * scale;
    for (std::size_t i = 0; i < h.rows.size(); ++i, y += row + 3.0f * scale) {
        const auto &r = h.rows[i];
        const float alpha = r.out ? 0.45f : 1.0f;
        band(draw, ImVec2(left - 30.0f * scale, y), ImVec2(display.x, y + row), r.self ? 0.7f : 0.5f, -1);
        draw->AddRectFilled(ImVec2(right + 8.0f * scale, y + 4.0f * scale), ImVec2(right + 12.0f * scale, y + row - 4.0f * scale), with_alpha(r.color, alpha));
        if (r.self) draw->AddRectFilledMultiColor(ImVec2(left - 30.0f * scale, y + row - 2.0f * scale), ImVec2(right, y + row),
                                                  with_alpha(accent, 0.0f), with_alpha(accent, 0.9f), with_alpha(accent, 0.9f), with_alpha(accent, 0.0f));
        const float text_y = y + (row - 18.0f * scale) * 0.5f;
        const auto place = std::to_string(i + 1);
        soft_text(draw, heading, 18.0f * scale, ImVec2(left, text_y - 1.0f * scale), with_alpha(r.up ? accent : IM_COL32(170, 175, 185, 255), alpha), place);
        if (r.up) diamond(draw, ImVec2(left + 30.0f * scale, y + row * 0.5f), 6.0f * scale, accent);
        soft_text(draw, bold, 18.0f * scale, ImVec2(left + 42.0f * scale, text_y), with_alpha(IM_COL32(255, 255, 255, 255), alpha), r.name);
        soft_text(draw, bold, 18.0f * scale, ImVec2(right - text_width(bold, 18.0f * scale, r.value), text_y), with_alpha(IM_COL32(255, 255, 255, 255), alpha),
                  r.value);
        if (r.out) draw->AddLine(ImVec2(left + 40.0f * scale, y + row * 0.5f), ImVec2(left + 44.0f * scale + text_width(bold, 18.0f * scale, r.name), y + row * 0.5f),
                                 with_alpha(theme::danger, 0.9f), std::max(1.0f, 2.0f * scale));
    }
}

// The middle of the screen: the countdown, the latest callout as a toast, the line being skated,
// the placing controls as button prompts and the out-of-area warning.
void draw_centre(ImDrawList *draw, HudState &st, float scale) {
    auto &s = state();
    const auto &h = st.hud;
    auto *title = s.menu.title ? s.menu.title : ImGui::GetFont();
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float mid = display.x * 0.5f;
    // Countdown: each number pops in.
    // Neon: a clean ring that drains with each second, a white number with a neon glow, popping in.
    if (h.clock.size() == 1) {
        const float t = static_cast<float>(std::fmod(ImGui::GetTime(), 1.0));
        const float pop = std::max(0.0f, 1.0f - t * 5.0f), grow = 1.0f + 0.18f * pop * pop;
        const ImVec2 c(mid, display.y * 0.32f);
        const float r = 74.0f * scale, line = std::max(2.0f, 3.5f * scale);
        draw->AddCircle(c, r, IM_COL32(255, 255, 255, 22), 96, std::max(1.0f, 1.5f * scale)); // the track, barely there
        const float left = 1.0f - t, start = -1.5707963f, end = start + 6.2831853f * left;
        if (left > 0.01f) {
            neon_arc(draw, c, r, start, end, neon_blue, line);
            neon_dot(draw, ImVec2(c.x + std::cos(end) * r, c.y + std::sin(end) * r), line * 1.6f, neon_blue);
        }
        const float size = 104.0f * scale * grow;
        neon_text(draw, title, size, ImVec2(c.x - text_width(title, size, h.clock) * 0.5f, c.y - size * 0.56f), neon_blue, h.clock);
        spaced_text(draw, heading, 15.0f * scale, c.x, c.y - r - 34.0f * scale, with_alpha(neon_blue, 0.9f), "GET READY", 4.0f * scale);
    }
    // The latest callout: a toast under the location plate, sliding in and fading out.
    if (h.banner_serial != st.banner_serial) {
        st.banner_serial = h.banner_serial;
        st.banner_at = Clock::now();
    }
    const float age = seconds_since(st.banner_at);
    if (!h.banner.empty() && age < 3.6f) {
        const float in = std::min(1.0f, age / 0.18f), out = age < 3.0f ? 1.0f : (3.6f - age) / 0.6f, alpha = in * out;
        const bool go = h.banner == "GO!";
        if (go) { // GO! in neon where the countdown was, growing out as it fades
            const float size = 110.0f * scale * (1.0f + 0.25f * (1.0f - out));
            neon_text(draw, title, size, ImVec2(mid - text_width(title, size, h.banner) * 0.5f, display.y * 0.32f - size * 0.56f),
                      with_alpha(neon_go, alpha), h.banner);
        }
        const float size = 26.0f * scale, w = text_width(heading, size, h.banner);
        const float y = 178.0f * scale - (1.0f - in) * 14.0f * scale;
        if (!go) {
        band(draw, ImVec2(mid - w * 0.5f - 160.0f * scale, y - 8.0f * scale), ImVec2(mid + w * 0.5f + 160.0f * scale, y + size + 10.0f * scale), 0.6f * alpha, 0);
        diamond(draw, ImVec2(mid - w * 0.5f - 22.0f * scale, y + size * 0.55f), 9.0f * scale, with_alpha(accent, alpha));
        soft_text(draw, heading, size, ImVec2(mid - w * 0.5f, y), with_alpha(IM_COL32(255, 255, 255, 255), alpha), h.banner);
        }
    }
    float bottom = display.y - 175.0f * scale;
    // The placing controls, the game's way: a D-pad glyph lit in its direction, then the action.
    if (!h.prompts.empty() || !h.hint.empty()) {
        const float glyph = 30.0f * scale, gap = 26.0f * scale, label_size = 18.0f * scale, key_size = 13.0f * scale;
        float total = 0;
        for (const auto &p : h.prompts) total += glyph + 10.0f * scale + std::max(text_width(bold, label_size, p.label), text_width(bold, key_size, p.key)) + gap;
        total -= gap;
        const float y = display.y - 92.0f * scale;
        band(draw, ImVec2(mid - total * 0.5f - 200.0f * scale, y - 44.0f * scale), ImVec2(mid + total * 0.5f + 200.0f * scale, y + 46.0f * scale), 0.62f, 0);
        if (!h.hint.empty())
            soft_text(draw, heading, 20.0f * scale, ImVec2(mid - text_width(heading, 20.0f * scale, h.hint) * 0.5f, y - 38.0f * scale), IM_COL32(255, 255, 255, 255), h.hint);
        float x = mid - total * 0.5f;
        for (const auto &p : h.prompts) {
            dpad_glyph(draw, ImVec2(x + glyph * 0.5f, y + 14.0f * scale), glyph, p.dpad);
            const float tx = x + glyph + 10.0f * scale;
            soft_text(draw, bold, label_size, ImVec2(tx, y + 1.0f * scale), IM_COL32(255, 255, 255, 255), p.label);
            soft_text(draw, bold, key_size, ImVec2(tx, y + 21.0f * scale), IM_COL32(170, 175, 185, 255), p.key);
            x = tx + std::max(text_width(bold, label_size, p.label), text_width(bold, key_size, p.key)) + gap;
        }
        bottom = y - 70.0f * scale;
    }
    if (!h.line.empty()) {
        const float size = 30.0f * scale, w = text_width(heading, size, h.line);
        band(draw, ImVec2(mid - w * 0.5f - 120.0f * scale, bottom - 6.0f * scale), ImVec2(mid + w * 0.5f + 120.0f * scale, bottom + size + 8.0f * scale), 0.45f, 0);
        soft_text(draw, heading, size, ImVec2(mid - w * 0.5f, bottom), accent, h.line);
        bottom -= 50.0f * scale;
    }
    if (!h.warning.empty()) {
        const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f), size = 22.0f * scale;
        const float w = text_width(heading, size, h.warning);
        draw->AddRectFilledMultiColor(ImVec2(mid - w * 0.5f - 140.0f * scale, bottom - 6.0f * scale), ImVec2(mid, bottom + size + 8.0f * scale),
                                      IM_COL32(180, 20, 20, 0), IM_COL32(180, 20, 20, static_cast<int>(170 * pulse)),
                                      IM_COL32(180, 20, 20, static_cast<int>(170 * pulse)), IM_COL32(180, 20, 20, 0));
        draw->AddRectFilledMultiColor(ImVec2(mid, bottom - 6.0f * scale), ImVec2(mid + w * 0.5f + 140.0f * scale, bottom + size + 8.0f * scale),
                                      IM_COL32(180, 20, 20, static_cast<int>(170 * pulse)), IM_COL32(180, 20, 20, 0), IM_COL32(180, 20, 20, 0),
                                      IM_COL32(180, 20, 20, static_cast<int>(170 * pulse)));
        soft_text(draw, heading, size, ImVec2(mid - w * 0.5f, bottom), IM_COL32(255, 255, 255, 255), h.warning);
    }
}

// A picture standing in the world: its quad's centre, half its width along `side` (a unit vector)
// and half its height along `upward`. False when a corner is behind the camera or the picture is
// not loaded yet.
bool world_picture(const Camera &cam, GamePicture picture, const Vec3 &centre, const Vec3 &side, const Vec3 &upward, float half_w,
                   float half_h, ImU32 tint) {
    const auto at = [&](float s, float u) {
        return Vec3{centre[0] + side[0] * s * half_w + upward[0] * u * half_h, centre[1] + side[1] * s * half_w + upward[1] * u * half_h,
                    centre[2] + side[2] * s * half_w + upward[2] * u * half_h};
    };
    const auto a = cam.project(at(-1, 1)), b = cam.project(at(1, 1)), c = cam.project(at(1, -1)), d = cam.project(at(-1, -1));
    if (!a || !b || !c || !d) return false;
    const ImVec2 corners[4]{*a, *b, *c, *d};
    return draw_game_picture_quad(picture, corners, tint);
}
// A Deathrace gate the way skate.'s race Throwdown puts its checkpoints up: the game's own glowing
// hoop (its checkpoint effect's texture) standing across the route and facing along it, a soft halo
// behind it, and in the next gate the game's lightning bolt turning to the camera. False when the
// pictures are not loaded (the gate is drawn the older way).
bool checkpoint_hoop(const Camera &cam, const Vec3 &foot, std::array<float, 2> facing, float radius, ImU32 colour, float alpha, bool next,
                     float time) {
    // The hoop fills about 88% of its picture; it stands a little sunk into the ground, as the game's does.
    const float breathe = next ? 1.0f + 0.025f * std::sin(time * 3.2f) : 1.0f;
    const float extent = radius / 0.44f * breathe;
    const Vec3 centre{foot[0], foot[1] + radius * 0.9f, foot[2]};
    const Vec3 across{-facing[1], 0.0f, facing[0]}, upward{0.0f, 1.0f, 0.0f};
    // A wider, fainter copy first: the glow around the ring.
    if (!world_picture(cam, GamePicture::checkpoint_ring, centre, across, upward, extent * 1.12f, extent * 1.12f,
                       with_alpha(colour, (next ? 0.38f : 0.2f) * alpha)))
        return false;
    world_picture(cam, GamePicture::checkpoint_ring, centre, across, upward, extent, extent, with_alpha(colour, 0.95f * alpha));
    // Its white-hot core.
    world_picture(cam, GamePicture::checkpoint_ring, centre, across, upward, extent * 0.985f, extent * 0.985f,
                  with_alpha(IM_COL32(255, 255, 255, 255), (next ? 0.75f : 0.4f) * alpha));
    if (next) {
        // The bolt, always turned to the camera, bobbing gently in the middle of the hoop.
        const Vec3 face_side{cam.right[0], cam.right[1], cam.right[2]}, face_up{cam.up[0], cam.up[1], cam.up[2]};
        const Vec3 middle{centre[0], centre[1] + 0.15f * radius * std::sin(time * 2.0f), centre[2]};
        const float bolt = radius * 0.55f;
        world_picture(cam, GamePicture::checkpoint_bolt, middle, face_side, face_up, bolt * 1.15f, bolt * 1.15f, with_alpha(colour, 0.45f));
        world_picture(cam, GamePicture::checkpoint_bolt, middle, face_side, face_up, bolt, bolt, IM_COL32(255, 255, 255, 240));
    }
    return true;
}

// A Deathrace route, built like an event the game itself put up: at each gate two round pillars
// (a billboard shaded light in the middle, dark at its edges) hold a banner reading START,
// CP 2 or a chequered FINISH, with arrows on the ground pointing the way through. The start is
// blue, checkpoints orange, the finish white. The next gate stands in a soft column of light and
// its arrows run; gates already passed fade back. Each gate faces the way it was placed.
void draw_route(ImDrawList *draw, const Camera &cam, const ModesHud &h, float scale, ImFont *font) {
    const auto count = h.points.size();
    const float pillar_r = 0.28f, top = 4.4f, banner_low = 3.4f;
    // Each gate as wide as it was set (a wide street needs a wide gate).
    const auto half_of = [&](std::size_t i) { return std::clamp(i < h.point_widths.size() ? h.point_widths[i] : h.radius, 1.5f, 40.0f); };
    const float time = static_cast<float>(ImGui::GetTime());
    const auto up = [](const Vec3 &v, float y) { return Vec3{v[0], v[1] + y, v[2]}; };
    // Which way gate i faces: its own facing when it has one, else along the route.
    const auto facing = [&](std::size_t i) {
        if (i < h.point_yaws.size()) {
            const float yaw = h.point_yaws[i] * 3.14159265f / 180.0f;
            return std::array<float, 2>{std::sin(yaw), std::cos(yaw)};
        }
        const auto from = i + 1 < count ? i : (i > 0 ? i - 1 : i), to = i + 1 < count ? i + 1 : i;
        const float dx = h.points[to][0] - h.points[from][0], dz = h.points[to][2] - h.points[from][2], length = std::hypot(dx, dz);
        return length > 0.01f ? std::array<float, 2>{dx / length, dz / length} : std::array<float, 2>{0, 1};
    };
    // A round pillar: a camera-facing strip, light down the middle and dark at its edges.
    const auto pillar = [&](const Vec3 &foot, ImU32 colour, float alpha) {
        const Vec3 side{cam.right[0] * pillar_r, 0, cam.right[2] * pillar_r};
        const auto c = ImGui::ColorConvertU32ToFloat4(colour);
        const auto shade = [&](float k, float a) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(1.0f, c.x * k), std::min(1.0f, c.y * k), std::min(1.0f, c.z * k), a * alpha));
        };
        const Vec3 l{foot[0] - side[0], foot[1], foot[2] - side[2]}, r{foot[0] + side[0], foot[1], foot[2] + side[2]};
        const Vec3 left_half[]{l, foot, up(foot, top), up(l, top)}, right_half[]{foot, r, up(r, top), up(foot, top)};
        const ImU32 edge = shade(0.45f, 0.95f), centre = shade(1.15f, 0.98f);
        const ImU32 lh[]{edge, centre, centre, edge}, rh[]{centre, edge, edge, centre};
        fill_world(draw, cam, left_half, lh);
        fill_world(draw, cam, right_half, rh);
        // A white cap and a dark foot ring make it read as solid.
        glow_path(draw, cam, {up(l, top), up(r, top)}, IM_COL32(255, 255, 255, 255), 0.05f, alpha);
    };
    for (std::size_t i = 0; i < count; ++i) {
        const auto &p = h.points[i];
        const float distance = cam.distance(p);
        if (distance > 600.0f) continue;
        const bool next = static_cast<int>(i) == h.next_point, done = h.next_point >= 0 && static_cast<int>(i) < h.next_point;
        const bool preview = h.placing && i + 1 == count;
        const bool start = i == 0, finish = i + 1 == count && count >= 2 && !h.placing;
        const ImU32 colour = preview ? placing_colour : finish ? IM_COL32(245, 245, 245, 255) : start ? start_blue : accent;
        const float alpha = done ? 0.3f : 1.0f;
        const auto d = facing(i);
        const float half = half_of(i), sx = -d[1] * half, sz = d[0] * half;
        const Vec3 left{p[0] + sx, p[1], p[2] + sz}, right{p[0] - sx, p[1], p[2] - sz};
        if (next) {
            // A soft column of light rising from the gate.
            const Vec3 column[]{left, right, up(right, 30.0f), up(left, 30.0f)};
            const ImU32 glow = with_alpha(colour, 0.22f), none = with_alpha(colour, 0.0f), shades[]{glow, glow, none, none};
            fill_world(draw, cam, column, shades);
        }
        // skate.'s own checkpoint hoop when its pictures are loaded; else pillars and a banner.
        const float hoop_radius = std::clamp(half, 1.5f, 9.0f);
        const bool hoop = checkpoint_hoop(cam, p, d, hoop_radius, colour, alpha, next || (start && h.next_point < 0), time);
        if (hoop) {
            glow_path(draw, cam, {up(left, 0.04f), up(right, 0.04f)}, colour, 0.07f, alpha);
            if (const auto mid = cam.project(up(p, hoop_radius * 1.9f + 0.8f))) {
                const std::string name = start ? "START" : finish ? "FINISH" : std::format("CHECKPOINT {}", i);
                const float size = std::clamp(0.55f * cam.focal / std::max(distance, 1.0f), 10.0f * scale, 60.0f * scale);
                const float w = text_width(font, size, name);
                soft_text(draw, font, size, ImVec2(mid->x - w * 0.5f, mid->y - size), with_alpha(IM_COL32(255, 255, 255, 255), alpha), name);
                if (next || (start && h.next_point < 0)) {
                    const auto text = std::format("{:.0f} m", distance);
                    const float metres_size = std::max(12.0f * scale, size * 0.5f), sw = text_width(font, metres_size, text);
                    soft_text(draw, font, metres_size, ImVec2(mid->x - sw * 0.5f, mid->y + 2.0f * scale), with_alpha(colour, 0.95f), text);
                }
            }
            if (!done) {
                const float run = next ? std::fmod(time * 1.2f, 1.0f) : 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float along = -3.0f + (k + run) * 2.0f, w = 1.1f;
                    const Vec3 tip{p[0] + d[0] * (along + 0.9f), p[1] + 0.05f, p[2] + d[1] * (along + 0.9f)};
                    const Vec3 l2{p[0] + d[0] * along - d[1] * w, p[1] + 0.05f, p[2] + d[1] * along + d[0] * w};
                    const Vec3 r2{p[0] + d[0] * along + d[1] * w, p[1] + 0.05f, p[2] + d[1] * along - d[0] * w};
                    const float fade = next ? 1.0f - std::abs(along) / 4.0f : 0.6f;
                    glow_path(draw, cam, {l2, tip, r2}, colour, 0.1f, std::clamp(fade, 0.2f, 1.0f));
                }
            }
            continue;
        }
        pillar(left, colour, alpha);
        pillar(right, colour, alpha);
        // The banner across the top: a lit face with white trim.
        const Vec3 banner[]{up(left, banner_low), up(right, banner_low), up(right, top), up(left, top)};
        const auto c = ImGui::ColorConvertU32ToFloat4(colour);
        const ImU32 lower = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x * 0.7f, c.y * 0.7f, c.z * 0.7f, 0.95f * alpha));
        const ImU32 upper = ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(1.0f, c.x * 1.1f), std::min(1.0f, c.y * 1.1f), std::min(1.0f, c.z * 1.1f), 0.95f * alpha));
        const ImU32 banner_shade[]{lower, lower, upper, upper};
        fill_world(draw, cam, banner, banner_shade);
        if (finish) {
            const auto at = [&](float t, float y) {
                return Vec3{left[0] + (right[0] - left[0]) * t, left[1] + y, left[2] + (right[2] - left[2]) * t};
            };
            const int columns = 12;
            for (int k = 0; k < columns; ++k)
                for (int rowi = 0; rowi < 2; ++rowi) {
                    if ((k + rowi) % 2) continue;
                    const float t0 = static_cast<float>(k) / columns, t1 = static_cast<float>(k + 1) / columns;
                    const float y0 = banner_low + rowi * (top - banner_low) * 0.5f, y1 = y0 + (top - banner_low) * 0.5f;
                    const Vec3 square[]{at(t0, y0), at(t1, y0), at(t1, y1), at(t0, y1)};
                    const ImU32 dark = IM_COL32(18, 18, 20, static_cast<int>(235 * alpha)), darks[]{dark, dark, dark, dark};
                    fill_world(draw, cam, square, darks);
                }
        }
        glow_path(draw, cam, {up(left, banner_low), up(right, banner_low)}, IM_COL32(255, 255, 255, 255), 0.035f, alpha);
        glow_path(draw, cam, {up(left, top), up(right, top)}, IM_COL32(255, 255, 255, 255), 0.035f, alpha);
        // The line to cross, and arrows on the ground pointing the way through.
        glow_path(draw, cam, {up(left, 0.04f), up(right, 0.04f)}, colour, 0.07f, alpha);
        if (!done) {
            const float run = next ? std::fmod(time * 1.2f, 1.0f) : 0.0f;
            for (int k = 0; k < 3; ++k) {
                const float along = -3.0f + (k + run) * 2.0f, w = 1.1f;
                const Vec3 tip{p[0] + d[0] * (along + 0.9f), p[1] + 0.05f, p[2] + d[1] * (along + 0.9f)};
                const Vec3 l2{p[0] + d[0] * along - d[1] * w, p[1] + 0.05f, p[2] + d[1] * along + d[0] * w};
                const Vec3 r2{p[0] + d[0] * along + d[1] * w, p[1] + 0.05f, p[2] + d[1] * along - d[0] * w};
                const float fade = next ? 1.0f - std::abs(along) / 4.0f : 0.6f;
                glow_path(draw, cam, {l2, tip, r2}, colour, 0.1f, std::clamp(fade, 0.2f, 1.0f));
            }
        }
        // The banner's lettering, sized as if printed on it.
        if (const auto mid = cam.project(up(p, (banner_low + top) * 0.5f))) {
            const std::string name = start ? "START" : finish ? "FINISH" : std::format("CP {}", i);
            const float size = std::clamp(0.62f * cam.focal / std::max(distance, 1.0f), 9.0f * scale, 72.0f * scale);
            const ImU32 ink = finish || preview ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 255);
            if (!finish) {
                const float w = text_width(font, size, name);
                soft_text(draw, font, size, ImVec2(mid->x - w * 0.5f, mid->y - size * 0.55f), with_alpha(ink, alpha), name);
            }
            if (next || (start && h.next_point < 0)) {
                const auto text = std::format("{:.0f} m", distance);
                const float label_size_px = std::max(12.0f * scale, size * 0.45f), w = text_width(font, label_size_px, text);
                if (const auto over = cam.project(up(p, top + 0.9f)))
                    soft_text(draw, font, label_size_px, ImVec2(over->x - w * 0.5f, over->y - label_size_px), IM_COL32(255, 255, 255, 230), text);
            }
        }
    }
    // The way on: chevrons along the ground from the skater toward the next gate, drifting forward.
    if (h.next_point >= 0 && static_cast<std::size_t>(h.next_point) < count && h.have_me) {
        const auto &to = h.points[static_cast<std::size_t>(h.next_point)];
        const auto &from = h.me;
        const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2], length = std::hypot(dx, dz);
        if (length > 6.0f) {
            const float ux = dx / length, uz = dz / length, run = std::fmod(time * 1.5f, 1.0f);
            for (int k = 0; k < 6; ++k) {
                const float along = 3.0f + (k + run) * 3.0f;
                if (along > length - 3.0f) break;
                const float t = along / length, y = from[1] + dy * t + 0.06f, w = 0.6f;
                const Vec3 tip{from[0] + ux * (along + 0.6f), y, from[2] + uz * (along + 0.6f)};
                const Vec3 l{from[0] + ux * along - uz * w, y, from[2] + uz * along + ux * w};
                const Vec3 r{from[0] + ux * along + uz * w, y, from[2] + uz * along - ux * w};
                glow_path(draw, cam, {l, tip, r}, accent, 0.07f, 0.55f * (1.0f - k / 6.0f));
            }
        }
    }
    (void)scale;
}

// The game's camera this frame, or nothing while its view is unknown.
std::optional<Camera> world_camera(const ModesHud &h) {
    if (!(h.vertical_fov > 1 && h.vertical_fov < 175)) return std::nullopt;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto &m = h.camera;
    return Camera{{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}, {m[12], m[13], m[14]},
                  display.y / (2.0f * std::tan(h.vertical_fov * 3.14159265f / 360.0f)), ImVec2(display.x * 0.5f, display.y * 0.5f)};
}

void draw_world(ImDrawList *draw, const ModesHud &h, float scale) {
    const auto view = world_camera(h);
    if (!view) return;
    const auto &cam = *view;
    const float thick = std::max(1.5f, 2.5f * scale);
    draw_boundary(draw, cam, h, scale);
    if (h.placing && (h.aim_ok || !h.aiming)) {
        // Where the next corner, checkpoint or centre goes: a beam over the spot on the ground.
        const auto &p = h.cursor;
        line3(draw, cam, p, {p[0], p[1] + 6.0f, p[2]}, with_alpha(placing_colour, 0.8f), thick * 2.0f);
        ring(draw, cam, p, 0.7f, with_alpha(theme::white, 0.9f), thick);
    }
    // Graffiti: every tag painted where it was skated, in its holder's colour.
    for (const auto &tag : h.tags) draw_tag(draw, cam, tag);
    // Checkpoints and spots.
    auto *bold = state().menu.bold ? state().menu.bold : ImGui::GetFont();
    if (h.route) {
        draw_route(draw, cam, h, scale, bold);
        return;
    }
    for (std::size_t i = 0; i < h.points.size(); ++i) {
        const auto &p = h.points[i];
        const bool next = static_cast<int>(i) == h.next_point;
        const bool done = h.next_point >= 0 && static_cast<int>(i) < h.next_point;
        const ImU32 owner = i < h.point_colors.size() ? h.point_colors[i] : 0;
        const ImU32 colour = next ? theme::bar : done ? with_alpha(theme::good, 0.5f) : owner ? owner : with_alpha(theme::white, 0.7f);
        if (owner) filled_ring(draw, cam, p, h.radius, with_alpha(owner, 0.25f));
        ring(draw, cam, p, h.radius, colour, next ? thick * 1.6f : thick);
        if (next) line3(draw, cam, p, {p[0], p[1] + 12.0f, p[2]}, with_alpha(theme::bar, 0.7f), thick * 2.0f);
        if (const auto label = cam.project({p[0], p[1] + (next ? 12.5f : 3.0f), p[2]})) {
            const float d = cam.distance(p);
            centred(draw, bold, 18.0f * scale, label->x, label->y - 20.0f * scale, colour,
                    std::format("{}  {:.0f} m", i + 1, d));
        }
    }
}

// A face button as the game draws it: a filled disc with its letter (A, or the PlayStation cross).
void face_button(ImDrawList *draw, ImFont *font, ImVec2 c, float r) {
    draw->AddCircleFilled(c, r, IM_COL32(18, 20, 26, 230), 24);
    draw->AddCircle(c, r, IM_COL32(90, 200, 90, 255), 24, std::max(1.0f, r * 0.14f));
    const float size = r * 1.25f, w = text_width(font, size, "A");
    draw->AddText(font, size, ImVec2(c.x - w * 0.5f, c.y - size * 0.52f), IM_COL32(120, 230, 120, 255), "A");
}
// "(A) / X hold (or J)  JOIN": the join prompt on a drop's card and on the announcement (A / X is
// skate.'s push, so a pad joins on a hold).
void join_prompt(ImDrawList *draw, ImFont *font, ImVec2 at, float scale, float alpha) {
    const float r = 11.0f * scale, size = 16.0f * scale;
    face_button(draw, font, ImVec2(at.x + r, at.y + r), r);
    float x = at.x + r * 2 + 6 * scale;
    const auto put = [&](const std::string &text, ImU32 colour) {
        soft_text(draw, font, size, ImVec2(x, at.y + r - size * 0.55f), with_alpha(colour, alpha), text);
        x += text_width(font, size, text);
    };
    put("/ X hold (or J)  ", IM_COL32(190, 195, 205, 255));
    put("JOIN", IM_COL32(255, 255, 255, 255));
}

// The flag on a game's spot, the way skate. marks a throwdown: a tall pole and a banner in the
// game's colour rippling in the wind, a white hem and the mode's name across it (the diamond when
// too far to read). The banner flies out to the camera's side so it is never seen edge on.
void draw_flag(ImDrawList *draw, const Camera &cam, const Vec3 &p, ImU32 colour, float time, ImFont *font, const std::string &name) {
    constexpr float pole = 6.0f, cloth_h = 1.8f, cloth_w = 3.6f;
    constexpr int segments = 14;
    const float distance = std::max(1.0f, cam.distance(p));
    const float px = cam.focal / distance; // pixels per metre at the flag
    if (px * pole < 4.0f) return;            // too far to make out: the beam marks it
    const float rl = std::max(1e-4f, std::hypot(cam.right[0], cam.right[2]));
    const float bl = std::max(1e-4f, std::hypot(cam.back[0], cam.back[2]));
    const Vec3 r{cam.right[0] / rl, 0, cam.right[2] / rl}, b{cam.back[0] / bl, 0, cam.back[2] / bl};
    const Vec3 top{p[0], p[1] + pole, p[2]};
    const auto shade = [](ImU32 c, float f) {
        const auto ch = [&](int shift) { return static_cast<ImU32>(std::clamp(((c >> shift) & 0xff) * f, 0.0f, 255.0f)) << shift; };
        return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
    };
    // The cloth: each strip along it moved in and out of the wind's wave, lit by how it faces.
    std::array<Vec3, segments + 1> upper{}, lower{};
    std::array<ImU32, segments + 1> tint{};
    for (int k = 0; k <= segments; ++k) {
        const float x = cloth_w * k / segments, out = x / cloth_w, phase = time * 4.0f - x * 2.2f;
        const float wave = std::sin(phase) * 0.28f * out, droop = 0.18f * out * out;
        const Vec3 at{top[0] + r[0] * x + b[0] * wave, top[1] - droop, top[2] + r[2] * x + b[2] * wave};
        upper[k] = at;
        lower[k] = {at[0], at[1] - cloth_h, at[2]};
        tint[k] = shade(colour, 0.72f + 0.28f * std::cos(phase));
    }
    for (int k = 0; k < segments; ++k) {
        const Vec3 quad[]{upper[k], upper[k + 1], lower[k + 1], lower[k]};
        const ImU32 shades[]{tint[k], tint[k + 1], tint[k + 1], tint[k]};
        fill_world(draw, cam, quad, shades);
    }
    // The hem and the free edge, white.
    const float hem = std::clamp(px * 0.09f, 1.0f, 5.0f);
    for (int k = 0; k < segments; ++k) line3(draw, cam, lower[k], lower[k + 1], IM_COL32(255, 255, 255, 230), hem);
    line3(draw, cam, upper[segments], lower[segments], IM_COL32(255, 255, 255, 160), hem * 0.6f);
    // The mode's name across the middle of the banner, as big as fits; the diamond when too small to read.
    const int mid = segments / 2;
    const Vec3 centre{(upper[mid][0] + lower[mid][0]) * 0.5f, (upper[mid][1] + lower[mid][1]) * 0.5f, (upper[mid][2] + lower[mid][2]) * 0.5f};
    if (const auto c = cam.project(centre)) {
        const float unit = text_width(font, 100.0f, name);
        const float size = std::min(px * cloth_h * 0.42f, unit > 0 ? px * cloth_w * 0.84f * 100.0f / unit : 0.0f);
        if (!name.empty() && size >= 8.0f) {
            const float w = text_width(font, size, name);
            const ImVec2 at(c->x - w * 0.5f, c->y - size * 0.55f);
            auto *face = crisp(font, size, name);
            draw->AddText(face, size, ImVec2(at.x + size * 0.06f, at.y + size * 0.06f), IM_COL32(0, 0, 0, 110), name.c_str());
            draw->AddText(face, size, at, IM_COL32(255, 255, 255, 245), name.c_str());
        } else if (px * 0.4f >= 3.0f) {
            diamond(draw, *c, px * 0.4f, IM_COL32(255, 255, 255, 235));
        }
    }
    // The pole, drawn over the cloth's near edge, with a cap.
    line3(draw, cam, p, {top[0], top[1] + 0.15f, top[2]}, IM_COL32(225, 228, 235, 255), std::clamp(px * 0.07f, 1.5f, 7.0f));
    if (const auto cap = cam.project({top[0], top[1] + 0.2f, top[2]}))
        draw->AddCircleFilled(*cap, std::clamp(px * 0.09f, 2.0f, 9.0f), IM_COL32(255, 255, 255, 255), 12);
}

// Other players' games, the way skate. shows a throwdown drop: a beam of light standing on the
// game's spot, seen from across the map, a ring pulsing on the ground and a card over it with the
// mode, the host, how many are in and how far it is. The one the join button would take lights up.
void draw_offers(ImDrawList *draw, const ModesHud &h, float scale) {
    const auto view = h.offers.empty() ? std::nullopt : world_camera(h);
    if (!view) return;
    const auto &cam = *view;
    auto &s = state();
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const float time = static_cast<float>(ImGui::GetTime());
    for (const auto &offer : h.offers) {
        if (!offer.has_at) continue;
        const auto &p = offer.at;
        const float distance = cam.distance(p);
        if (distance > 1500.0f) continue;
        const ImU32 colour = offer.open ? accent : IM_COL32(150, 155, 165, 255);
        // The beam: wide and soft, with a bright core, fading up into the sky.
        for (const auto &[width, alpha] : {std::pair{1.6f, 0.10f}, std::pair{0.7f, 0.18f}, std::pair{0.22f, 0.55f}}) {
            const Vec3 side{cam.right[0] * width, 0, cam.right[2] * width};
            const Vec3 beam[]{{p[0] - side[0], p[1], p[2] - side[2]}, {p[0] + side[0], p[1], p[2] + side[2]},
                              {p[0] + side[0], p[1] + 80.0f, p[2] + side[2]}, {p[0] - side[0], p[1] + 80.0f, p[2] - side[2]}};
            const ImU32 low = with_alpha(colour, alpha), high = with_alpha(colour, 0.0f), shades[]{low, low, high, high};
            fill_world(draw, cam, beam, shades);
        }
        // The ring on the ground, pulsing outward.
        const float pulse = std::fmod(time * 0.6f, 1.0f);
        ring(draw, cam, p, 2.5f, with_alpha(colour, 0.9f), std::max(2.0f, 3.0f * scale));
        ring(draw, cam, p, 2.5f + pulse * 3.0f, with_alpha(colour, 0.6f * (1.0f - pulse)), std::max(1.5f, 2.0f * scale));
        draw_flag(draw, cam, p, colour, time, heading, offer.mode);
        // The card, a fixed size on screen above the flag.
        const auto anchor = cam.project({p[0], p[1] + 7.2f, p[2]});
        if (!anchor) continue;
        const float title_size = 22.0f * scale, line_size = 15.0f * scale;
        const std::string where = std::format("{:.0f} m", distance);
        const float w = std::max({text_width(heading, title_size, offer.mode) + 46.0f * scale, text_width(bold, line_size, offer.host + "   " + where),
                                  text_width(bold, line_size, offer.detail), offer.target ? 190.0f * scale : 0.0f}) + 28.0f * scale;
        const float card_h = (offer.target ? 104.0f : 74.0f) * scale;
        const ImVec2 a(anchor->x - w * 0.5f, anchor->y - card_h), b(anchor->x + w * 0.5f, anchor->y);
        const ImU32 back = IM_COL32(10, 12, 16, offer.target ? 220 : 185);
        draw->AddRectFilled(a, b, back, 3.0f * scale);
        draw->AddRectFilled(a, ImVec2(b.x, a.y + 4.0f * scale), colour, 3.0f * scale, ImDrawFlags_RoundCornersTop);
        if (offer.target) draw->AddRect(a, b, with_alpha(colour, 0.9f), 3.0f * scale, 0, std::max(1.5f, 2.0f * scale));
        // A pointer down to the spot.
        draw->AddTriangleFilled(ImVec2(anchor->x - 8 * scale, b.y), ImVec2(anchor->x + 8 * scale, b.y), ImVec2(anchor->x, b.y + 9 * scale), back);
        diamond(draw, ImVec2(a.x + 20 * scale, a.y + 22 * scale), 8.0f * scale, colour);
        soft_text(draw, heading, title_size, ImVec2(a.x + 36 * scale, a.y + 10 * scale), IM_COL32(255, 255, 255, 255), offer.mode);
        soft_text(draw, bold, line_size, ImVec2(a.x + 14 * scale, a.y + 38 * scale), IM_COL32(200, 205, 215, 255), offer.host);
        soft_text(draw, bold, line_size, ImVec2(b.x - 14 * scale - text_width(bold, line_size, where), a.y + 38 * scale), colour, where);
        soft_text(draw, bold, line_size, ImVec2(a.x + 14 * scale, a.y + 55 * scale), IM_COL32(160, 165, 175, 255), offer.detail);
        if (offer.target && h.can_join) join_prompt(draw, bold, ImVec2(a.x + 14 * scale, a.y + 76 * scale), scale, 1.0f);
    }
}

// The announcement of a new game: a toast sliding in at the top, the game's own way (a dark band,
// the accent diamond, the mode in capitals), with the join prompt while it can be joined.
void draw_invite(ImDrawList *draw, const ModesHud &h, float scale) {
    if (h.invite.empty() || h.invite_fade <= 0.01f) return;
    auto &s = state();
    auto *title = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float alpha = std::clamp(h.invite_fade, 0.0f, 1.0f), slide = (1.0f - std::min(1.0f, alpha * 3.0f)) * 30.0f * scale;
    const float y = 110.0f * scale - slide, mid = display.x * 0.5f;
    const std::string head = "NEW GAME";
    const float size = 15.0f * scale, big = 24.0f * scale;
    const float w = std::max(text_width(title, big, h.invite), 260.0f * scale);
    band(draw, ImVec2(mid - w * 0.5f - 160 * scale, y - 10 * scale), ImVec2(mid + w * 0.5f + 160 * scale, y + 86 * scale), 0.7f * alpha, 0);
    draw->AddRectFilled(ImVec2(mid - w * 0.5f, y - 10 * scale), ImVec2(mid + w * 0.5f, y - 7 * scale), with_alpha(accent, alpha));
    diamond(draw, ImVec2(mid - text_width(bold, size, head) * 0.5f - 14 * scale, y + 9 * scale), 6.0f * scale, with_alpha(accent, alpha));
    soft_text(draw, bold, size, ImVec2(mid - text_width(bold, size, head) * 0.5f, y + 1 * scale), with_alpha(accent, alpha), head);
    soft_text(draw, title, big, ImVec2(mid - text_width(title, big, h.invite) * 0.5f, y + 22 * scale),
              IM_COL32(255, 255, 255, static_cast<int>(255 * alpha)), h.invite);
    if (h.can_join) join_prompt(draw, bold, ImVec2(mid - 105.0f * scale, y + 54 * scale), scale, alpha);
}

// Skate Tag. Whoever is it wears a small neon crown floating over their head (a glowing band with
// five points, bobbing and slowly turning), seen by everyone. Off screen, arrows at the edge point
// the way: for the one who is it, to every other player; for everyone else, to it. Being it, the
// screen's edges glow faintly pink.
void draw_tag_players(ImDrawList *draw, const ModesHud &h, float scale) {
    if (h.players.empty()) return;
    const auto view = world_camera(h);
    if (!view) return;
    const auto &cam = *view;
    auto &s = state();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float time = static_cast<float>(ImGui::GetTime());
    // Skate Tag's it is pink and crowned; Infection's reapers are toxic green, under a halo.
    // Hide & Seek's seekers burn orange under a halo too.
    const ImU32 neon = h.hide ? IM_COL32(255, 130, 20, 255) : h.infection ? IM_COL32(110, 255, 70, 255) : IM_COL32(255, 60, 200, 255);
    const std::string it_label = h.hide ? "SEEKER" : h.infection ? "REAPER" : "IT";
    bool me_it = false;
    for (const auto &p : h.players)
        if (p.self && p.it) me_it = true;
    if (me_it) {
        const float pulse = 0.55f + 0.45f * std::sin(time * 3.0f), edge = 120.0f * scale;
        const ImU32 glow = with_alpha(neon, 0.16f * pulse), none = with_alpha(neon, 0.0f);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, edge), glow, glow, none, none);
        draw->AddRectFilledMultiColor(ImVec2(0, display.y - edge), display, none, none, glow, glow);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(edge, display.y), glow, none, none, glow);
        draw->AddRectFilledMultiColor(ImVec2(display.x - edge, 0), display, none, glow, glow, none);
    }
    for (const auto &p : h.players) {
        if (!p.it) continue;
        // The crown, a little over the head.
        const float bob = std::sin(time * 2.4f) * 0.05f, turn = time * 1.2f, r = 0.17f;
        const Vec3 c{p.at[0], p.at[1] + 2.15f + bob, p.at[2]};
        const auto ring = [&](float up) {
            std::vector<Vec3> points;
            for (int k = 0; k <= 24; ++k) {
                const float a = turn + 6.2831853f * k / 24;
                points.push_back({c[0] + std::cos(a) * r, c[1] + up, c[2] + std::sin(a) * r});
            }
            return points;
        };
        glow_path(draw, cam, ring(0.0f), neon, 0.02f, 1.0f);
        glow_path(draw, cam, ring(0.045f), neon, 0.012f, 0.8f);
        if (h.infection || h.hide) { // a halo, no points
            const float d = cam.distance(c);
            if (!p.self && d > 20.0f)
                if (const auto label = cam.project({c[0], c[1] + 0.4f, c[2]})) {
                    const std::string text = std::format("{}  {:.0f} m", it_label, d);
                    const float size = 16.0f * scale, w = text_width(bold, size, text);
                    soft_text(draw, bold, size, ImVec2(label->x - w * 0.5f, label->y - size), neon, text);
                }
            continue;
        }
        std::vector<Vec3> points;
        for (int k = 0; k <= 10; ++k) {
            const float a = turn + 6.2831853f * k / 10, up = k % 2 == 0 ? 0.19f : 0.045f;
            points.push_back({c[0] + std::cos(a) * r, c[1] + up, c[2] + std::sin(a) * r});
        }
        glow_path(draw, cam, points, neon, 0.016f, 1.0f);
        for (std::size_t k = 0; k < 10; k += 2) // a bright jewel on each point
            if (const auto tip = cam.project(points[k])) {
                const float size = std::clamp(0.025f * cam.focal / std::max(cam.distance(points[k]), 0.5f), 1.5f, 6.0f);
                draw->AddCircleFilled(*tip, size, IM_COL32(255, 230, 250, 255), 10);
            }
        // Far away, the crown is small: "IT" and how far, over it.
        const float d = cam.distance(c);
        if (!p.self && d > 20.0f)
            if (const auto label = cam.project({c[0], c[1] + 0.5f, c[2]})) {
                const std::string text = std::format("IT  {:.0f} m", d);
                const float size = 16.0f * scale, w = text_width(bold, size, text);
                soft_text(draw, bold, size, ImVec2(label->x - w * 0.5f, label->y - size), neon, text);
            }
    }
    // Arrows at the screen's edge to those who matter and are out of sight.
    const float inset = 70.0f * scale;
    const ImVec2 centre(display.x * 0.5f, display.y * 0.5f), half(display.x * 0.5f - inset, display.y * 0.5f - inset);
    for (const auto &p : h.players) {
        if (p.self || (me_it ? p.it : !p.it)) continue;
        const Vec3 chest{p.at[0], p.at[1] + 1.2f, p.at[2]};
        if (const auto on = cam.project(chest); on && std::abs(on->x - centre.x) < half.x && std::abs(on->y - centre.y) < half.y) continue;
        const auto v = cam.view(chest);
        float dx = v.side, dy = -v.height;
        if (std::abs(dx) < 0.01f && std::abs(dy) < 0.01f) dy = 1;
        const float length = std::max(1e-4f, std::sqrt(dx * dx + dy * dy));
        dx /= length;
        dy /= length;
        const float t = std::min(std::abs(dx) > 1e-4f ? half.x / std::abs(dx) : 1e9f, std::abs(dy) > 1e-4f ? half.y / std::abs(dy) : 1e9f);
        const ImVec2 at(centre.x + dx * t, centre.y + dy * t);
        const ImU32 colour = p.it ? neon : IM_COL32(255, 255, 255, 230);
        const float size = 16.0f * scale;
        const ImVec2 tip(at.x + dx * size, at.y + dy * size), left(at.x - dy * size * 0.6f, at.y + dx * size * 0.6f),
            right(at.x + dy * size * 0.6f, at.y - dx * size * 0.6f);
        draw->AddTriangleFilled(tip, right, left, IM_COL32(0, 0, 0, 140));
        draw->AddTriangleFilled(ImVec2(tip.x - dx * 2, tip.y - dy * 2), right, left, colour);
        const std::string text = std::format("{}  {:.0f} m", p.it ? it_label : p.name, cam.distance(p.at));
        const float text_size = 15.0f * scale, w = text_width(bold, text_size, text);
        soft_text(draw, bold, text_size, ImVec2(at.x - dx * size * 1.6f - w * 0.5f, at.y - dy * size * 1.6f - text_size * 0.5f), colour, text);
    }
}

// A gold crown: a band with three points, a jewel on each point.
void crown(ImDrawList *draw, ImVec2 centre, float size, ImU32 gold) {
    const float w = size, h = size * 0.62f, x = centre.x - w * 0.5f, y = centre.y - h * 0.5f;
    const ImVec2 outline[]{{x, y + h},
                           {x, y + h * 0.25f},
                           {x + w * 0.25f, y + h * 0.6f},
                           {x + w * 0.5f, y},
                           {x + w * 0.75f, y + h * 0.6f},
                           {x + w, y + h * 0.25f},
                           {x + w, y + h}};
    draw->AddConcavePolyFilled(outline, 7, gold);
    draw->AddPolyline(outline, 7, IM_COL32(120, 80, 0, 255), ImDrawFlags_Closed, std::max(1.0f, size * 0.04f));
    draw->AddRectFilled(ImVec2(x, y + h * 0.82f), ImVec2(x + w, y + h), IM_COL32(200, 140, 20, 255));
    for (const auto &tip : {outline[1], outline[3], outline[5]}) draw->AddCircleFilled(tip, size * 0.08f, IM_COL32(255, 70, 90, 255), 12);
}

// S.K.A.T.E.: the trick to copy, the way skate.'s own S.K.A.T.E. shows it, low in the middle of the
// screen: for each part a stick gate with its flick drawn on it (where to start, the way round, an
// arrow at the end, and a dot running the flick over and over), its name under it; a part with no
// flick (a grab, a grind, a manual) says what it is instead.
void draw_trick_diagram(ImDrawList *draw, const ModesHud &h, float scale) {
    if (h.trick_parts.empty()) return;
    auto &s = state();
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float r = 48.0f * scale, gap = 46.0f * scale, cell = r * 2.0f + gap;
    const float total = cell * static_cast<float>(h.trick_parts.size()) - gap;
    const float cy = display.y - 190.0f * scale, left = display.x * 0.5f - total * 0.5f;
    const float time = static_cast<float>(ImGui::GetTime());
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    band(draw, ImVec2(left - 160.0f * scale, cy - r - 52.0f * scale), ImVec2(left + total + 160.0f * scale, cy + r + 46.0f * scale), 0.55f, 0);
    const float title = 17.0f * scale;
    diamond(draw, ImVec2(display.x * 0.5f - text_width(bold, title, h.trick_title) * 0.5f - 14.0f * scale, cy - r - 30.0f * scale), 5.0f * scale, accent);
    soft_text(draw, bold, title, ImVec2(display.x * 0.5f - text_width(bold, title, h.trick_title) * 0.5f, cy - r - 40.0f * scale), accent, h.trick_title);
    for (std::size_t i = 0; i < h.trick_parts.size(); ++i) {
        const auto &part = h.trick_parts[i];
        const ImVec2 c(left + r + cell * static_cast<float>(i), cy);
        if (i > 0) soft_text(draw, heading, 28.0f * scale, ImVec2(c.x - r - gap * 0.5f - 8.0f * scale, cy - 16.0f * scale), white, "+");
        // The stick's gate.
        draw->AddCircleFilled(c, r, IM_COL32(12, 14, 18, 190), 48);
        draw->AddCircle(c, r, IM_COL32(255, 255, 255, 120), 48, std::max(1.5f, 2.0f * scale));
        draw->AddCircleFilled(c, 3.0f * scale, IM_COL32(255, 255, 255, 90), 12);
        if (!part.path.empty()) {
            // The gesture's x is to the left: on screen that is minus x, mirrored for a goofy stance.
            const float side = h.goofy ? 1.0f : -1.0f, reach = r * 0.82f;
            std::vector<ImVec2> points;
            for (const auto &p : part.path) points.emplace_back(c.x + side * p[0] * reach, c.y + p[1] * reach);
            // Rounded through its corners a little, for the look of a flick rather than a polygon.
            std::vector<ImVec2> smooth;
            for (std::size_t k = 0; k + 1 < points.size(); ++k)
                for (int step = 0; step < 8; ++step) {
                    const float t = static_cast<float>(step) / 8.0f;
                    smooth.emplace_back(points[k].x + (points[k + 1].x - points[k].x) * t, points[k].y + (points[k + 1].y - points[k].y) * t);
                }
            smooth.push_back(points.back());
            for (int round = 0; round < 6; ++round)
                for (std::size_t k = 1; k + 1 < smooth.size(); ++k)
                        smooth[k] = ImVec2((smooth[k - 1].x + smooth[k].x * 2.0f + smooth[k + 1].x) * 0.25f,
                                           (smooth[k - 1].y + smooth[k].y * 2.0f + smooth[k + 1].y) * 0.25f);
            const float thick = std::max(2.5f, 5.0f * scale);
            for (std::size_t k = 0; k + 1 < smooth.size(); ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(smooth.size() - 1);
                draw->AddLine(smooth[k], smooth[k + 1], with_alpha(accent, 0.45f + 0.55f * t), thick);
            }
            // Where to start, and the arrow at the end.
            draw->AddCircleFilled(smooth.front(), 6.0f * scale, white, 16);
            draw->AddCircle(smooth.front(), 6.0f * scale, accent, 16, std::max(1.0f, 1.5f * scale));
            const auto &tip = smooth.back(), &back = smooth[smooth.size() >= 4 ? smooth.size() - 4 : 0];
            float dx = tip.x - back.x, dy = tip.y - back.y;
            const float length = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
            dx /= length;
            dy /= length;
            const float a = 13.0f * scale, w = 8.0f * scale;
            draw->AddTriangleFilled(ImVec2(tip.x + dx * a * 0.5f, tip.y + dy * a * 0.5f), ImVec2(tip.x - dx * a * 0.5f - dy * w, tip.y - dy * a * 0.5f + dx * w),
                                    ImVec2(tip.x - dx * a * 0.5f + dy * w, tip.y - dy * a * 0.5f - dx * w), accent);
            // The stick doing it: a dot running the flick, then a pause.
            const float cycle = std::fmod(time + 0.37f * static_cast<float>(i), 1.6f) / 0.9f;
            if (cycle <= 1.0f) {
                const float at = cycle * static_cast<float>(smooth.size() - 1);
                const auto k = std::min(static_cast<std::size_t>(at), smooth.size() - 2);
                const float f = at - static_cast<float>(k);
                const ImVec2 dot(smooth[k].x + (smooth[k + 1].x - smooth[k].x) * f, smooth[k].y + (smooth[k + 1].y - smooth[k].y) * f);
                draw->AddCircleFilled(dot, 9.0f * scale, IM_COL32(255, 255, 255, 230), 20);
                draw->AddCircle(dot, 9.0f * scale, IM_COL32(20, 20, 20, 200), 20, std::max(1.0f, 1.5f * scale));
            }
        } else {
            const float size = 15.0f * scale;
            soft_text(draw, heading, size, ImVec2(c.x - text_width(heading, size, part.kind) * 0.5f, c.y - size * 0.55f), accent, part.kind);
        }
        // The part's name under its gate, kept to the cell.
        const float name_size = 15.0f * scale;
        std::string name = part.name;
        while (name.size() > 1 && text_width(bold, name_size, name) > cell - 6.0f * scale) name.pop_back();
        soft_text(draw, bold, name_size, ImVec2(c.x - text_width(bold, name_size, name) * 0.5f, c.y + r + 8.0f * scale), white, name);
    }
}

// "1ST", "2ND", "3RD", "4TH" ... "11TH", "21ST".
std::string ordinal(std::size_t n) {
    const auto tens = n % 100, ones = n % 10;
    const char *end = tens >= 11 && tens <= 13 ? "TH" : ones == 1 ? "ST" : ones == 2 ? "ND" : ones == 3 ? "RD" : "TH";
    return std::to_string(n) + end;
}

ImU32 mix(ImU32 c, ImU32 to, float t) {
    const auto ch = [&](int shift) {
        const float a = static_cast<float>((c >> shift) & 0xff), b = static_cast<float>((to >> shift) & 0xff);
        return static_cast<ImU32>(std::clamp(a + (b - a) * t, 0.0f, 255.0f)) << shift;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}
// A podium place as a polished metal plate: slanted ends, shaded like metal from a bright top to a
// dark band and a lit lower edge, a rim, a glint sweeping across every few seconds, the place stamped
// in, and (gold only) a sparkle twinkling at its corner.
void shiny_badge(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 metal, ImFont *font, float size, const std::string &text, float alpha,
                 float time, float offset, bool sparkle) {
    const float slant = (b.y - a.y) * 0.28f, h = b.y - a.y;
    const ImU32 white = IM_COL32(255, 255, 255, 255), black = IM_COL32(0, 0, 0, 255);
    // Left and right edges of the plate at height y.
    const auto lx = [&](float y) { return a.x + slant * (1.0f - (y - a.y) / h); };
    const auto rx = [&](float y) { return b.x - slant * ((y - a.y) / h); };
    // The metal's shading down the plate: highlight, body, the dark band, the lit lower edge.
    constexpr int strips = 16;
    const auto tone = [&](float t) {
        if (t < 0.18f) return mix(metal, white, 0.75f - t * 2.2f);
        if (t < 0.55f) return mix(metal, white, 0.12f * (0.55f - t) / 0.37f);
        if (t < 0.82f) return mix(metal, black, 0.38f * (t - 0.55f) / 0.27f);
        return mix(mix(metal, black, 0.38f), white, (t - 0.82f) * 1.6f);
    };
    for (int k = 0; k < strips; ++k) {
        const float y0 = a.y + h * k / strips, y1 = a.y + h * (k + 1) / strips;
        const ImU32 top = with_alpha(tone(static_cast<float>(k) / strips), alpha), bottom = with_alpha(tone(static_cast<float>(k + 1) / strips), alpha);
        draw->AddRectFilledMultiColor(ImVec2(lx(y1), y0), ImVec2(rx(y0), y1), top, top, bottom, bottom);
        // The slanted ends: a triangle each side of the strip's rectangle.
        draw->AddTriangleFilled(ImVec2(lx(y0), y0), ImVec2(lx(y1), y0), ImVec2(lx(y1), y1), top);
        draw->AddTriangleFilled(ImVec2(rx(y0), y0), ImVec2(rx(y1), y1), ImVec2(rx(y0), y1), bottom);
    }
    // The glint: a slanted band of light crossing the plate every few seconds.
    const float period = 3.2f, sweep = std::fmod(time + offset, period) / 0.9f;
    if (sweep < 1.0f) {
        const float w = b.x - a.x, cx = a.x - w * 0.3f + sweep * w * 1.6f, band_w = w * 0.12f;
        draw->PushClipRect(a, b, true);
        for (int k = 0; k < strips; ++k) {
            const float y0 = a.y + h * k / strips, y1 = a.y + h * (k + 1) / strips;
            const float l = std::max(cx - band_w - (y0 - a.y) * 0.6f, lx(y0)), r = std::min(cx + band_w - (y0 - a.y) * 0.6f, rx(y0));
            if (r > l) {
                const float m = (l + r) * 0.5f;
                const ImU32 lit = IM_COL32(255, 255, 255, static_cast<int>(150 * alpha)), clear = IM_COL32(255, 255, 255, 0);
                draw->AddRectFilledMultiColor(ImVec2(l, y0), ImVec2(m, y1), clear, lit, lit, clear);
                draw->AddRectFilledMultiColor(ImVec2(m, y0), ImVec2(r, y1), lit, clear, clear, lit);
            }
        }
        draw->PopClipRect();
    }
    // The rim: dark outside, a bright line just inside the top.
    const ImVec2 outline[]{ImVec2(a.x + slant, a.y), ImVec2(b.x, a.y), ImVec2(b.x - slant, b.y), ImVec2(a.x, b.y)};
    draw->AddPolyline(outline, 4, with_alpha(mix(metal, black, 0.55f), alpha), ImDrawFlags_Closed, std::max(1.0f, h * 0.045f));
    draw->AddLine(ImVec2(a.x + slant * 0.92f + 2.0f, a.y + h * 0.07f), ImVec2(b.x - 3.0f, a.y + h * 0.07f), with_alpha(white, 0.75f * alpha),
                  std::max(1.0f, h * 0.035f));
    // The place, stamped in: a light edge under dark lettering.
    const float tw = text_width(font, size, text);
    const ImVec2 at((a.x + b.x) * 0.5f - tw * 0.5f, a.y + (h - size) * 0.5f);
    draw->AddText(font, size, ImVec2(at.x, at.y + std::max(1.0f, size * 0.05f)), with_alpha(mix(metal, white, 0.7f), alpha), text.c_str());
    draw->AddText(font, size, at, with_alpha(mix(metal, black, 0.72f), alpha), text.c_str());
    // A four-pointed sparkle at the top corner, twinkling.
    if (sparkle) {
        const float tw2 = 0.5f + 0.5f * std::sin((time + offset) * 3.1f);
        const float r = h * (0.18f + 0.16f * tw2);
        const ImVec2 c(b.x - slant * 0.2f - h * 0.08f, a.y + h * 0.1f);
        const ImU32 star = with_alpha(white, alpha * (0.5f + 0.5f * tw2));
        draw->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.22f, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r * 0.22f, c.y), star);
        draw->AddQuadFilled(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y - r * 0.22f), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r * 0.22f), star);
    }
}

// The end of a game, the way skate. shows a challenge's results: a column down the left of the
// screen on dark bands fading out to the right, leaving the middle clear. The winner first, crowned
// in gold; second in silver and third in bronze under them; everyone else smaller below. Each row
// slides in after the one above it; the local player's is underlined in the accent.
void draw_results(ImDrawList *draw, const ModesHud &h, float scale) {
    auto &s = state();
    auto *title = s.menu.title ? s.menu.title : ImGui::GetFont();
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    // When these results came up: a gap in drawing them means a new game's.
    static double shown_at = 0, last_drawn = -10;
    const double now = ImGui::GetTime();
    if (now - last_drawn > 0.5) shown_at = now;
    last_drawn = now;
    const float age = static_cast<float>(now - shown_at);
    const auto appear = [&](float delay) { // 0..1, eased, from `delay` seconds in
        const float t = std::clamp((age - delay) / 0.35f, 0.0f, 1.0f);
        return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    };
    constexpr ImU32 metal[]{IM_COL32(255, 196, 40, 255), IM_COL32(205, 212, 222, 255), IM_COL32(214, 132, 62, 255)};
    const ImU32 white = IM_COL32(255, 255, 255, 255), grey = IM_COL32(170, 175, 185, 255);

    // The left of the screen darkened a little, so the column reads over any scene.
    const float shade = appear(0.0f);
    draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x * 0.55f, display.y), IM_COL32(0, 0, 0, static_cast<int>(150 * shade)),
                                  IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, static_cast<int>(150 * shade)));

    const float x = 64.0f * scale, width = 580.0f * scale, right = x + width;
    float y = display.y * 0.17f;
    // The header: the mode in the accent, RESULTS large, an accent line fading out.
    {
        const float a = appear(0.0f), slide = (1.0f - a) * -40.0f * scale;
        std::string mode = h.title;
        for (auto &c : mode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        diamond(draw, ImVec2(x + slide + 7.0f * scale, y + 11.0f * scale), 6.0f * scale, with_alpha(accent, a));
        soft_text(draw, heading, 18.0f * scale, ImVec2(x + slide + 22.0f * scale, y), with_alpha(accent, a), mode);
        soft_text(draw, title, 56.0f * scale, ImVec2(x + slide, y + 22.0f * scale), with_alpha(white, a), "RESULTS");
        draw->AddRectFilledMultiColor(ImVec2(x + slide, y + 86.0f * scale), ImVec2(right + slide, y + 89.0f * scale), with_alpha(accent, a),
                                      with_alpha(accent, 0.0f), with_alpha(accent, 0.0f), with_alpha(accent, a));
        y += 104.0f * scale;
    }

    const std::size_t shown = std::min<std::size_t>(h.rows.size(), 8);
    std::size_t self_place = 0;
    for (std::size_t i = 0; i < h.rows.size(); ++i)
        if (h.rows[i].self) self_place = i + 1;
    for (std::size_t i = 0; i < shown; ++i) {
        const auto &row = h.rows[i];
        const bool podium = i < 3;
        const float tall = (i == 0 ? 104.0f : podium ? 68.0f : 40.0f) * scale;
        const float a = appear(0.25f + 0.12f * static_cast<float>(i)), slide = (1.0f - a) * -60.0f * scale;
        const float left = x + slide, end = right + slide;
        const ImU32 badge = podium ? metal[i] : grey;
        // The band, darker for the podium, and a stripe in the place's metal at its edge.
        band(draw, ImVec2(0, y), ImVec2(end + 120.0f * scale, y + tall), (podium ? 0.72f : 0.55f) * a, 1);
        draw->AddRectFilled(ImVec2(left - 18.0f * scale, y), ImVec2(left - 12.0f * scale, y + tall), with_alpha(badge, a));
        // The place: shiny plates for the podium ("1ST" gold under the winner's crown, "2ND" silver,
        // "3RD" bronze), plain grey text for the rest.
        const std::string place = ordinal(i + 1);
        const float place_size = (i == 0 ? 34.0f : podium ? 26.0f : 19.0f) * scale;
        const float place_w = (i == 0 ? 112.0f : podium ? 92.0f : 58.0f) * scale;
        const float t = static_cast<float>(now);
        if (i == 0) {
            crown(draw, ImVec2(left + place_w * 0.5f, y + 2.0f * scale), 40.0f * scale, with_alpha(metal[0], a));
            shiny_badge(draw, ImVec2(left, y + 44.0f * scale), ImVec2(left + place_w - 6.0f * scale, y + tall - 6.0f * scale), metal[0], heading,
                        place_size, place, a, t, 0.0f, true);
        } else if (podium) {
            shiny_badge(draw, ImVec2(left, y + 9.0f * scale), ImVec2(left + place_w - 8.0f * scale, y + tall - 9.0f * scale), badge, heading,
                        place_size, place, a, t, 0.35f * static_cast<float>(i), false);
        } else {
            soft_text(draw, heading, place_size, ImVec2(left, y + (tall - place_size) * 0.5f), with_alpha(badge, a), place);
        }
        // The player's colour, their name and their score. The score is placed first, on the right; the
        // name gets the room left of it (with YOU after it for the local player): a long name is drawn
        // smaller, down to 70%, and past that cut short with "...", so the two never run into each other.
        auto *name_font = i == 0 ? heading : bold;
        const float value_size = (i == 0 ? 26.0f : podium ? 21.0f : 17.0f) * scale;
        const float value_w = text_width(heading, value_size, row.value);
        const float name_x = left + place_w + 8.0f * scale, text_x = name_x + 14.0f * scale;
        const float you_w = row.self ? text_width(bold, 13.0f * scale, "YOU") + 10.0f * scale : 0.0f;
        const float room = std::max(40.0f * scale, end - value_w - 18.0f * scale - text_x - you_w);
        float name_size = (i == 0 ? 34.0f : podium ? 25.0f : 18.0f) * scale;
        if (const float w = text_width(name_font, name_size, row.name); w > room) name_size = std::max(name_size * 0.7f, name_size * room / w);
        std::string name = row.name;
        while (name.size() > 1 && text_width(name_font, name_size, name + "...") > room) name.pop_back();
        if (name != row.name) name += "...";
        const float name_y = y + (tall - name_size) * 0.5f - (i == 0 ? 2.0f * scale : 0.0f);
        draw->AddRectFilled(ImVec2(name_x, name_y + name_size * 0.15f), ImVec2(name_x + 5.0f * scale, name_y + name_size * 0.95f), with_alpha(row.color, a));
        soft_text(draw, name_font, name_size, ImVec2(text_x, name_y), with_alpha(white, a), name);
        soft_text(draw, heading, value_size, ImVec2(end - value_w, y + (tall - value_size) * 0.5f), with_alpha(i == 0 ? metal[0] : white, a), row.value);
        if (row.self) {
            draw->AddRectFilledMultiColor(ImVec2(left - 12.0f * scale, y + tall - 3.0f * scale), ImVec2(end + 60.0f * scale, y + tall),
                                          with_alpha(accent, a), with_alpha(accent, 0.0f), with_alpha(accent, 0.0f), with_alpha(accent, a));
            const float you_x = text_x + text_width(name_font, name_size, name) + 10.0f * scale;
            soft_text(draw, bold, 13.0f * scale, ImVec2(you_x, name_y + name_size - 15.0f * scale), with_alpha(accent, a), "YOU");
        }
        y += tall + (podium ? 8.0f : 4.0f) * scale;
    }
    // Where the local player finished, when it was off the podium, and when it all closes.
    const float a = appear(0.4f + 0.12f * static_cast<float>(shown));
    y += 10.0f * scale;
    if (self_place > 3)
        soft_text(draw, heading, 20.0f * scale, ImVec2(x, y), with_alpha(white, a), "YOU FINISHED " + ordinal(self_place));
    if (self_place > 3) y += 30.0f * scale;
    soft_text(draw, bold, 15.0f * scale, ImVec2(x, y), with_alpha(grey, a), std::format("Closing in {} s", (h.closing_ms + 999) / 1000));
}
} // namespace

bool modes_hud_pending() {
    auto &h = hud_state();
    h.hud = {};
    if (const auto feed = modes_hud_feed.load()) {
        try { h.hud = feed(); } catch (...) { h.hud = {}; }
    }
    return h.hud.active || !h.hud.offers.empty() || !h.hud.invite.empty();
}

// Hide & Seek, for a seeker while the others hide: the screen goes black, the seconds left counting
// down in neon in the middle.
void draw_blind(ImDrawList *draw, const ModesHud &h, float scale) {
    auto &s = state();
    auto *heading = s.menu.heading ? s.menu.heading : ImGui::GetFont();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float time = static_cast<float>(ImGui::GetTime());
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 255));
    const ImU32 orange = IM_COL32(255, 130, 20, 255);
    const float pulse = 0.5f + 0.5f * std::sin(time * 3.0f);
    // A slow ring breathing round the count.
    const ImVec2 c(display.x * 0.5f, display.y * 0.47f);
    neon_arc(draw, c, 150.0f * scale, 0.0f, 6.2831853f, with_alpha(orange, 0.35f + 0.25f * pulse), 2.0f * scale);
    const float size = 150.0f * scale, w = text_width(heading, size, h.blind);
    neon_text(draw, heading, size, ImVec2(c.x - w * 0.5f, c.y - size * 0.55f), orange, h.blind);
    spaced_text(draw, bold, 30.0f * scale, c.x, c.y - 250.0f * scale, IM_COL32(255, 255, 255, 235), "YOU'RE SEEKING", 6.0f * scale);
    spaced_text(draw, bold, 17.0f * scale, c.x, c.y + 185.0f * scale, IM_COL32(200, 200, 205, 200), "EYES CLOSED. EVERYONE IS HIDING...", 3.0f * scale);
}
// Hide & Seek's hot/cold meter at the bottom of the screen: ice blue far away, white hot close by,
// beating faster the closer it gets. A hider being closed in on also feels it at the screen's edges.
void draw_heat(ImDrawList *draw, const ModesHud &h, float scale) {
    if (h.heat < 0) return;
    auto &s = state();
    auto *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    const auto display = ImGui::GetIO().DisplaySize;
    const float time = static_cast<float>(ImGui::GetTime());
    const auto mix = [](ImU32 a, ImU32 b, float t) {
        const auto x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
    };
    const ImU32 cold = IM_COL32(60, 170, 255, 255), warm = IM_COL32(255, 170, 30, 255), hot = IM_COL32(255, 40, 40, 255);
    const ImU32 colour = h.heat < 0.5f ? mix(cold, warm, h.heat * 2.0f) : mix(warm, hot, (h.heat - 0.5f) * 2.0f);
    const float beat = 0.5f + 0.5f * std::sin(time * (2.0f + h.heat * 12.0f));
    const float width = 420.0f * scale, height = 14.0f * scale;
    const ImVec2 a(display.x * 0.5f - width * 0.5f, display.y - 150.0f * scale), b(a.x + width, a.y + height);
    draw->AddRectFilled(ImVec2(a.x - 6 * scale, a.y - 6 * scale), ImVec2(b.x + 6 * scale, b.y + 6 * scale), IM_COL32(0, 0, 0, 140), 12.0f * scale);
    // The scale itself, cold to hot, dimmed past the reading.
    const float mid = a.x + width * 0.5f;
    draw->AddRectFilledMultiColor(a, ImVec2(mid, b.y), with_alpha(cold, 0.35f), with_alpha(warm, 0.35f), with_alpha(warm, 0.35f), with_alpha(cold, 0.35f));
    draw->AddRectFilledMultiColor(ImVec2(mid, a.y), b, with_alpha(warm, 0.35f), with_alpha(hot, 0.35f), with_alpha(hot, 0.35f), with_alpha(warm, 0.35f));
    const float x = a.x + width * h.heat;
    draw->PushClipRect(a, ImVec2(x, b.y), true);
    draw->AddRectFilledMultiColor(a, ImVec2(mid, b.y), cold, warm, warm, cold);
    draw->AddRectFilledMultiColor(ImVec2(mid, a.y), b, warm, hot, hot, warm);
    draw->PopClipRect();
    neon_dot(draw, ImVec2(x, a.y + height * 0.5f), (6.0f + 4.0f * beat * h.heat) * scale, colour);
    const std::string label = h.heat_label;
    const float size = 30.0f * scale, w = text_width(bold, size, label);
    neon_text(draw, bold, size, ImVec2(display.x * 0.5f - w * 0.5f, a.y - 50.0f * scale), with_alpha(colour, 0.75f + 0.25f * beat), label);
    spaced_text(draw, bold, 13.0f * scale, display.x * 0.5f, b.y + 10.0f * scale, IM_COL32(220, 220, 225, 200),
                h.heat_seeking ? "NEAREST HIDER" : "NEAREST SEEKER", 3.0f * scale);
    if (!h.heat_seeking && h.heat > 0.6f) { // a hider with a seeker close: the edges throb red
        const float strength = (h.heat - 0.6f) / 0.4f * (0.25f + 0.35f * beat), edge = 160.0f * scale;
        const ImU32 glow = with_alpha(hot, strength), none = with_alpha(hot, 0.0f);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, edge), glow, glow, none, none);
        draw->AddRectFilledMultiColor(ImVec2(0, display.y - edge), display, none, none, glow, glow);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(edge, display.y), glow, none, none, glow);
        draw->AddRectFilledMultiColor(ImVec2(display.x - edge, 0), display, none, glow, glow, none);
    }
}

void draw_modes_hud() {
    auto &h = hud_state();
    if (!h.hud.active && h.hud.offers.empty() && h.hud.invite.empty()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.8f, 2.0f);
    auto *draw = ImGui::GetBackgroundDrawList();
    // Other players' games show whether or not the player is in one.
    draw_offers(draw, h.hud, scale);
    draw_invite(draw, h.hud, scale);
    if (!h.hud.active) return;
    draw_world(draw, h.hud, scale);
    if (!h.hud.blind.empty()) { // a blind seeker sees only the count and the board
        draw_blind(draw, h.hud, scale);
        draw_panel(draw, h, scale);
        return;
    }
    draw_tag_players(draw, h.hud, scale);
    draw_heat(draw, h.hud, scale);
    if (h.hud.results) {
        draw_results(draw, h.hud, scale);
        return;
    }
    draw_panel(draw, h, scale);
    draw_centre(draw, h, scale);
    draw_trick_diagram(draw, h.hud, scale);
    if (h.hud.placing && h.hud.aiming) {
        // The quick-drop reticle: white with a dot when it finds the ground, red when it does not.
        const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
        const float r = 14.0f * scale, t = std::max(1.5f, 2.0f * scale);
        const ImU32 ink = h.hud.aim_ok ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 80, 70, 235), shade = IM_COL32(0, 0, 0, 120);
        draw->AddCircle(c, r + t, shade, 32, t * 2.0f);
        draw->AddCircle(c, r, ink, 32, t);
        for (const auto &d : {ImVec2(1, 0), ImVec2(-1, 0), ImVec2(0, 1), ImVec2(0, -1)})
            draw->AddLine(ImVec2(c.x + d.x * r * 0.45f, c.y + d.y * r * 0.45f), ImVec2(c.x + d.x * r * 1.6f, c.y + d.y * r * 1.6f), ink, t);
        if (h.hud.aim_ok) draw->AddCircleFilled(c, 2.5f * scale, ink, 12);
    }
}
} // namespace dingosdk::overlay::detail
