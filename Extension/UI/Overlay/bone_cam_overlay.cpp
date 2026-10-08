#include "overlay_internal.h"
#include "Extension/Modes/bone_sprites.h"
#include "Extension/UI/skate_theme.h"
#include "bone_cam_3d.h"
#include <wincodec.h>
#include <future>

// The Bone Cam (Extension/Modes/bone_cam.h). The bone sprites are built into ReSkate.dll
// (Extension/Modes/Assets/bones.png), decoded once on a background thread and copied into the ImGui
// atlas like the chat emotes, so they need no texture of their own. During a bail the screen dims
// to an X-ray blue and each bone is drawn between the skater's joints; broken bones turn red and
// crack. Background draw list, under the game modes HUD and ReSkate's menus; it takes no input.

namespace dingosdk::overlay {
namespace {
std::atomic<BoneCamFeed> bone_cam_feed{};
}
void set_bone_cam_feed(BoneCamFeed feed) noexcept { bone_cam_feed.store(feed); }
} // namespace dingosdk::overlay

using namespace dingosdk::overlay::detail;
namespace dingosdk::overlay::detail {
namespace {
namespace theme = dingosdk::skate_theme;
using Microsoft::WRL::ComPtr;
using modes::bone_sprites;
using Vec3 = std::array<float, 3>;

struct Pixels {
    std::vector<unsigned char> rgba;
    UINT width{}, height{};
};
struct Sprites {
    std::mutex mutex;
    std::future<Pixels> pending;
    std::optional<Pixels> decoded;
    ImFontAtlas *atlas{};
    std::array<int, bone_sprites.size()> rects{};
    bool filled{};
};
Sprites &sprites() {
    static auto *value = new Sprites;
    return *value;
}
Pixels decode_embedded() {
    Pixels result;
    HMODULE module{};
    static const int anchor = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &module))
        return result;
    const auto resource = FindResourceW(module, L"BONE_ATLAS", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto handle = resource ? LoadResource(module, resource) : nullptr;
    const auto *data = handle ? static_cast<const BYTE *>(LockResource(handle)) : nullptr;
    if (!data) return result;
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width{}, height{};
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE *>(data), SizeofResource(module, resource))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) &&
        width == static_cast<UINT>(modes::bone_atlas_width) && height == static_cast<UINT>(modes::bone_atlas_height) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom))) {
        result.rgba.resize(static_cast<std::size_t>(width) * height * 4);
        if (SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(result.rgba.size()), result.rgba.data()))) {
            result.width = width;
            result.height = height;
        } else {
            result.rgba.clear();
        }
    }
    if (com) CoUninitialize();
    return result;
}
} // namespace

void start_bone_sprites() {
    auto &s = sprites();
    std::lock_guard lock(s.mutex);
    if (s.pending.valid() || s.decoded) return;
    s.pending = std::async(std::launch::async, [] {
        try { return decode_embedded(); } catch (...) { return Pixels{}; }
    });
}

std::size_t reserve_bone_sprites(ImFontAtlas &atlas, std::chrono::milliseconds wait) noexcept {
    try {
        auto &s = sprites();
        std::lock_guard lock(s.mutex);
        s.atlas = nullptr;
        s.filled = false;
        if (!s.decoded) {
            if (!s.pending.valid() || s.pending.wait_for(wait) != std::future_status::ready) return 0;
            s.decoded = s.pending.get();
        }
        if (s.decoded->rgba.empty()) return 0;
        atlas.TexDesiredWidth = std::max(atlas.TexDesiredWidth, 2048);
        atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
        for (std::size_t i = 0; i < bone_sprites.size(); ++i) s.rects[i] = atlas.AddCustomRectRegular(bone_sprites[i].w, bone_sprites[i].h);
        s.atlas = &atlas;
        return bone_sprites.size();
    } catch (...) {
        return 0;
    }
}

void fill_bone_sprites(ImFontAtlas &atlas) noexcept {
    auto &s = sprites();
    std::lock_guard lock(s.mutex);
    if (s.atlas != &atlas || !s.decoded || s.decoded->rgba.empty()) return;
    unsigned char *pixels{};
    int width{}, height{};
    atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels) return;
    const auto &source = *s.decoded;
    for (std::size_t i = 0; i < bone_sprites.size(); ++i) {
        const auto &sprite = bone_sprites[i];
        const auto *rect = atlas.GetCustomRectByIndex(s.rects[i]);
        if (!rect || !rect->IsPacked() || rect->X + sprite.w > width || rect->Y + sprite.h > height) return;
        for (int y = 0; y < sprite.h; ++y)
            std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4,
                        source.rgba.data() + ((static_cast<std::size_t>(sprite.y) + y) * source.width + sprite.x) * 4,
                        static_cast<std::size_t>(sprite.w) * 4);
    }
    atlas.TexPixelsUseColors = true;
    s.filled = true;
}

namespace {
BoneCam &bone_cam_frame() {
    static BoneCam value;
    return value;
}
ImU32 faded(ImU32 colour, float alpha) {
    const auto a = static_cast<unsigned>(((colour >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}
void shadowed(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const char *text) {
    const auto alpha = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f;
    draw->AddText(font, size, ImVec2(at.x + size / 16, at.y + size / 16), faded(IM_COL32(0, 0, 0, 255), alpha * .7f), text);
    draw->AddText(font, size, at, colour, text);
}
} // namespace

bool bone_cam_pending() {
    auto &cam = bone_cam_frame();
    cam = {};
    if (const auto feed = bone_cam_feed.load()) {
        try { cam = feed(); } catch (...) { cam = {}; }
    }
    return cam.active || cam.effects;
}

void draw_bone_cam() {
    const auto &cam = bone_cam_frame();
    if (!cam.active && !cam.effects) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float k = std::clamp(display.y / 1080.0f, 0.8f, 2.0f), fade = cam.active ? cam.fade : 0.0f;
    auto *draw = ImGui::GetBackgroundDrawList();
    const bool viewable = cam.vertical_fov > 1 && cam.vertical_fov < 175;
    const auto &m = cam.camera;
    const Vec3 right{m[0], m[1], m[2]}, up{m[4], m[5], m[6]}, back{m[8], m[9], m[10]}, origin{m[12], m[13], m[14]};
    const float focal = viewable ? display.y / (2.0f * std::tan(cam.vertical_fov * 3.14159265f / 360.0f)) : 1.0f;
    const ImVec2 centre(display.x * .5f, display.y * .5f);
    struct Projected { ImVec2 at; float depth; };
    const auto project = [&](const Vec3 &p) {
        const Vec3 d{p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
        const float depth = -(d[0] * back[0] + d[1] * back[1] + d[2] * back[2]);
        const float side = d[0] * right[0] + d[1] * right[1] + d[2] * right[2], height = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        return Projected{depth > .05f ? ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth) : ImVec2{}, depth};
    };

    // Marks on the ground: a skid is a broad dark smear with a darker core where the body slid; a
    // scrape is a few thin pale scratches, as from a hand, knee or helmet dragged along concrete.
    if (viewable)
        for (const auto &mark : cam.marks) {
            for (std::size_t i = 1; i < mark.path.size(); ++i) {
                const auto &p0 = mark.path[i - 1], &p1 = mark.path[i];
                const float dx = p1[0] - p0[0], dz = p1[2] - p0[2], length = std::hypot(dx, dz);
                if (length < 0.01f) continue;
                const float nx = -dz / length, nz = dx / length;
                // Fresh at the start of the slide, thinning out where the body slowed.
                const float t = static_cast<float>(i) / mark.path.size(), taper = 1.0f - 0.5f * t;
                const auto stroke = [&](float offset, float width_m, ImU32 colour) {
                    const auto a = project({p0[0] + nx * offset, p0[1], p0[2] + nz * offset});
                    const auto b = project({p1[0] + nx * offset, p1[1], p1[2] + nz * offset});
                    if (a.depth <= .3f || b.depth <= .3f) return;
                    const float width = std::max(1.0f, width_m * focal / ((a.depth + b.depth) * .5f));
                    draw->AddLine(a.at, b.at, colour, width);
                };
                if (mark.skid) {
                    stroke(0, 0.34f * taper, faded(IM_COL32(28, 22, 18, 255), .35f * mark.alpha));
                    stroke(0, 0.16f * taper, faded(IM_COL32(18, 14, 12, 255), .45f * mark.alpha));
                } else {
                    for (const float offset : {-0.035f, 0.0f, 0.04f})
                        stroke(offset, 0.012f, faded(IM_COL32(228, 222, 210, 255), (offset == 0 ? .55f : .35f) * mark.alpha * taper));
                    stroke(0, 0.09f * taper, faded(IM_COL32(40, 32, 28, 255), .18f * mark.alpha));
                }
            }
        }

    // The hit: the screen's edges flash red.
    const auto draw_hit = [&] {
        if (cam.hit <= 0.01f) return;
        const float h = cam.hit, edge = 220 * k * (0.6f + 0.4f * h);
        const ImU32 red = faded(IM_COL32(200, 20, 10, 255), .55f * h), none = faded(IM_COL32(200, 20, 10, 255), 0);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, edge), red, red, none, none);
        draw->AddRectFilledMultiColor(ImVec2(0, display.y - edge), display, none, none, red, red);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(edge, display.y), red, none, none, red);
        draw->AddRectFilledMultiColor(ImVec2(display.x - edge, 0), display, none, red, red, none);
        draw->AddRectFilled(ImVec2(0, 0), display, faded(IM_COL32(255, 255, 255, 255), .12f * h * h));
    };
    // A concussion, after the X-ray: the edges of the view dim a little and drift slowly, with a
    // faint haze over everything. Deliberately light.
    if (cam.daze > 0.01f && !cam.active) {
        const float d = std::min(cam.daze, 1.0f), t = static_cast<float>(ImGui::GetTime());
        const float sway = std::sin(t * 1.1f) * 28.0f * k * d, lift = std::sin(t * 0.8f + 1.3f) * 14.0f * k * d;
        const float edge = (180.0f + 80.0f * d) * k;
        const ImU32 dim = IM_COL32(8, 10, 20, static_cast<int>(70 * d)), none = IM_COL32(8, 10, 20, 0);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, edge + lift), dim, dim, none, none);
        draw->AddRectFilledMultiColor(ImVec2(0, display.y - edge + lift), display, none, none, dim, dim);
        draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(edge + sway, display.y), dim, none, none, dim);
        draw->AddRectFilledMultiColor(ImVec2(display.x - edge + sway, 0), display, none, dim, dim, none);
        draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(235, 240, 255, static_cast<int>(12 * d)));
    }
    if (!cam.active || fade <= 0) {
        draw_hit();
        return;
    }
    // The X-ray film: the world dims to a deep blue with faint scan lines.
    draw->AddRectFilled(ImVec2(0, 0), display, faded(IM_COL32(4, 16, 40, 200), fade));
    for (float y = 0; y < display.y; y += 5 * k) draw->AddLine(ImVec2(0, y), ImVec2(display.x, y), faded(IM_COL32(120, 200, 255, 9), fade));

    auto &s = sprites();
    const bool textures = s.filled && s.atlas && ImGui::GetIO().Fonts == s.atlas && s.atlas->TexID;
    if (viewable && cam.pose.valid) {
        // The 3D skeleton inside the skater (bone_cam_3d.cpp); breaks show on the bones themselves.
        draw_xray_skeleton(draw, cam.pose, XrayView{right, up, back, origin, focal, centre}, fade);
    } else if (textures && viewable) {
        for (const auto &bone : cam.bones) {
            if (bone.sprite >= bone_sprites.size()) continue;
            const auto &sprite = bone_sprites[bone.sprite];
            const auto a = project(bone.a), b = project(bone.b);
            if (a.depth <= .2f || b.depth <= .2f) continue;
            // The sprite's axis (anchor a toward b) is laid along the bone on screen. Across the bone it
            // keeps the artwork's real size at that distance; along it, it follows the bone's projected
            // length, so a limb pointing at the camera foreshortens.
            const float ux = sprite.b[0] - sprite.a[0], uy = sprite.b[1] - sprite.a[1], length = std::sqrt(ux * ux + uy * uy);
            const float dx = b.at.x - a.at.x, dy = b.at.y - a.at.y, shown = std::sqrt(dx * dx + dy * dy);
            if (length < 1 || shown < .5f) continue;
            const float across = modes::bone_metres_per_pixel * focal / a.depth;
            const float along = std::clamp(shown / (length * across), .3f, 1.3f) * across;
            const ImVec2 u(ux / length, uy / length), U(dx / shown, dy / shown);
            // The art is drawn facing the viewer, so its +x is the skater's left. Seen from behind (or
            // turned), the bone is flipped across its own axis so that side lands on the skater's left.
            float flip = 1;
            if (const auto side = project({bone.a[0] + cam.left[0], bone.a[1] + cam.left[1], bone.a[2] + cam.left[2]});
                side.depth > .2f) {
                const ImVec2 left(side.at.x - a.at.x, side.at.y - a.at.y);
                // Where the sprite's +x lands on screen without a flip.
                const ImVec2 x_dir(U.x * u.x * along + U.y * u.y * across, U.y * u.x * along - U.x * u.y * across);
                if (x_dir.x * left.x + x_dir.y * left.y < 0) flip = -1;
            }
            const auto place = [&](float x, float y) {
                const float rx = x - sprite.a[0], ry = y - sprite.a[1];
                const float cu = rx * u.x + ry * u.y, cv = (-rx * u.y + ry * u.x) * flip;
                return ImVec2(a.at.x + U.x * cu * along - U.y * cv * across, a.at.y + U.y * cu * along + U.x * cv * across);
            };
            ImVec2 uv0, uv1;
            s.atlas->CalcCustomRectUV(s.atlas->GetCustomRectByIndex(s.rects[bone.sprite]), &uv0, &uv1);
            const ImVec2 p1 = place(0, 0), p2 = place(static_cast<float>(sprite.w), 0),
                         p3 = place(static_cast<float>(sprite.w), static_cast<float>(sprite.h)), p4 = place(0, static_cast<float>(sprite.h));
            const ImU32 tint = bone.hurt == 2 ? IM_COL32(255, 120, 105, 255) : IM_COL32(255, 255, 255, 240);
            // A soft glow under each bone, red for an injured one.
            const ImVec2 mid((p1.x + p3.x) * .5f, (p1.y + p3.y) * .5f);
            const auto grow = [&](ImVec2 p) { return ImVec2(mid.x + (p.x - mid.x) * 1.05f, mid.y + (p.y - mid.y) * 1.05f); };
            draw->AddImageQuad(s.atlas->TexID, grow(p1), grow(p2), grow(p3), grow(p4), uv0, ImVec2(uv1.x, uv0.y), uv1, ImVec2(uv0.x, uv1.y),
                               faded(bone.hurt == 2 ? IM_COL32(255, 60, 40, 255) : bone.hurt == 1 ? IM_COL32(255, 150, 40, 255) : IM_COL32(90, 180, 255, 255), .35f * fade));
            draw->AddImageQuad(s.atlas->TexID, p1, p2, p3, p4, uv0, ImVec2(uv1.x, uv0.y), uv1, ImVec2(uv0.x, uv1.y), faded(tint, fade));
            if (bone.hurt) {
                // The break: a jagged crack across the middle of the bone, and an impact burst when broken.
                const ImVec2 at((a.at.x + b.at.x) * .5f, (a.at.y + b.at.y) * .5f), n(-U.y, U.x);
                const float r = std::max(6.0f, across * 40);
                ImVec2 zig[7];
                for (int i = 0; i < 7; ++i) {
                    const float t = (i - 3) / 3.0f * r, w = (i % 2 ? 1 : -1) * r * .25f;
                    zig[i] = ImVec2(at.x + n.x * t + U.x * w, at.y + n.y * t + U.y * w);
                }
                draw->AddPolyline(zig, 7, faded(bone.hurt == 2 ? IM_COL32(255, 241, 201, 255) : IM_COL32(255, 170, 60, 255), fade), ImDrawFlags_None, std::max(1.5f, 2.5f * k));
                if (bone.hurt == 2) {
                    draw->AddCircleFilled(at, r * 1.4f, faded(IM_COL32(255, 60, 40, 70), fade), 24);
                    for (int i = 0; i < 10; ++i) {
                        const float t = i / 10.0f * 6.2831853f;
                        draw->AddLine(ImVec2(at.x + std::cos(t) * r * 1.5f, at.y + std::sin(t) * r * 1.5f),
                                      ImVec2(at.x + std::cos(t) * r * 2.4f, at.y + std::sin(t) * r * 2.4f),
                                      faded(IM_COL32(255, 80, 60, 220), fade), std::max(1.5f, 2.5f * k));
                    }
                }
            }
        }
    }
    // BONE CAM at the top, slow motion and the injuries on the left.
    auto &st = state();
    auto *title = st.menu.title ? st.menu.title : ImGui::GetFont();
    auto *heading = st.menu.heading ? st.menu.heading : ImGui::GetFont();
    auto *bold = st.menu.bold ? st.menu.bold : ImGui::GetFont();
    {
        const char *text = cam.preview ? "BONE CAM PREVIEW" : "BONE CAM";
        const auto extent = title->CalcTextSizeA(40 * k, FLT_MAX, 0, text);
        const ImVec2 at(display.x * .5f - extent.x * .5f, 40 * k);
        theme::rough_rect(draw, ImVec2(at.x - 24 * k, at.y - 8 * k), ImVec2(at.x + extent.x + 24 * k, at.y + extent.y + 10 * k),
                          faded(theme::tile, .9f * fade), 97u, k);
        shadowed(draw, title, 40 * k, at, faded(theme::white, fade), text);
    }
    float y = 120 * k;
    const float left = 32 * k;
    if (cam.preview) {
        const char *text = "PREVIEW";
        const auto extent = heading->CalcTextSizeA(22 * k, FLT_MAX, 0, text);
        draw->AddRectFilled(ImVec2(left, y), ImVec2(left + extent.x + 24 * k, y + extent.y + 12 * k), faded(IM_COL32(143, 208, 255, 255), fade));
        draw->AddText(heading, 22 * k, ImVec2(left + 12 * k, y + 6 * k), faded(IM_COL32(11, 29, 54, 255), fade), text);
        y += extent.y + 24 * k;
    }
    if (!cam.injuries.empty()) {
        const float width = 330 * k, row = 30 * k;
        shadowed(draw, heading, 18 * k, ImVec2(left + 2 * k, y), faded(theme::danger, fade), "INJURIES");
        y += 26 * k;
        for (std::size_t i = 0; i < cam.injuries.size() && i < 8; ++i) {
            const auto &injury = cam.injuries[i];
            theme::rough_rect(draw, ImVec2(left, y), ImVec2(left + width, y + row), faded(theme::tile, .9f * fade), static_cast<unsigned>(i * 7 + 2), k);
            draw->AddRectFilled(ImVec2(left, y), ImVec2(left + 5 * k, y + row), faded(injury.severe ? theme::danger : theme::warning, fade));
            const float text_y = y + (row - 16 * k) * .5f;
            shadowed(draw, bold, 16 * k, ImVec2(left + 14 * k, text_y), faded(theme::white, fade), injury.bone.c_str());
            const auto extent = bold->CalcTextSizeA(16 * k, FLT_MAX, 0, injury.what.c_str());
            shadowed(draw, bold, 16 * k, ImVec2(left + width - 12 * k - extent.x, text_y),
                     faded(injury.severe ? theme::danger : theme::warning, fade), injury.what.c_str());
            y += row + 4 * k;
        }
    }
    draw_hit();
}
} // namespace dingosdk::overlay::detail
