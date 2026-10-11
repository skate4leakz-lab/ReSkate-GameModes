#include "overlay_internal.h"
#include "Extension/Multiplayer/Steam/steam_social.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

// Players' Steam profile pictures for the game modes HUD and results: each one asked for is read
// from Steam (ready once Steam has it) into a cell of one sheet, which the render thread uploads
// as its own texture (upload_avatar_sheet in overlay_render.cpp) whenever a new picture lands.
namespace dingosdk::overlay::detail {
namespace {
constexpr UINT cell = 64, per_row = 8, sheet_side = cell * per_row; // 64 pictures
struct Sheet {
    std::mutex mutex;
    std::map<std::uint64_t, int> cells; // Steam ID -> cell, once its picture is in
    std::map<std::uint64_t, std::uint64_t> wanted; // Steam ID -> next time to ask Steam
    std::vector<unsigned char> rgba = std::vector<unsigned char>(std::size_t{sheet_side} * sheet_side * 4);
    bool dirty{};
    unsigned serial{};
    ImTextureID texture{};
    unsigned uploaded{};
};
Sheet &sheet() {
    static auto *value = new Sheet;
    return *value;
}
// A picture fitted into its cell, its corners left clear (drawn round anyway).
void put(Sheet &s, int index, const std::vector<std::uint8_t> &picture, std::uint32_t w, std::uint32_t h) {
    const UINT x0 = static_cast<UINT>(index) % per_row * cell, y0 = static_cast<UINT>(index) / per_row * cell;
    for (UINT y = 0; y < cell; ++y)
        for (UINT x = 0; x < cell; ++x) {
            const auto sx = std::min<std::uint32_t>(w - 1, x * w / cell), sy = std::min<std::uint32_t>(h - 1, y * h / cell);
            std::memcpy(&s.rgba[(std::size_t{y0 + y} * sheet_side + x0 + x) * 4], &picture[(std::size_t{sy} * w + sx) * 4], 4);
        }
}
}

void want_avatar(std::uint64_t steam_id) noexcept {
    // Steam IDs of real accounts (individual, public universe): a solo game's stand-in id is not one.
    if ((steam_id >> 52) != 0x011) return;
    auto &s = sheet();
    std::lock_guard lock(s.mutex);
    if (!s.cells.contains(steam_id)) s.wanted.try_emplace(steam_id, 0);
}

bool take_avatar_sheet(CardPixels &pixels) {
    auto &s = sheet();
    std::vector<std::uint64_t> ask;
    {
        std::lock_guard lock(s.mutex);
        const auto now = GetTickCount64();
        for (auto &[id, next] : s.wanted)
            if (now >= next && ask.size() < 4) {
                ask.push_back(id);
                next = now + 1000; // Steam is asked again a second later until it has the picture
            }
    }
    for (const auto id : ask) {
        std::vector<std::uint8_t> picture;
        std::uint32_t w{}, h{};
        if (!multiplayer::steam_avatar(id, picture, w, h)) continue;
        std::lock_guard lock(s.mutex);
        if (s.cells.size() >= std::size_t{per_row} * per_row) s.cells.clear(); // full: start the sheet over
        const int index = static_cast<int>(s.cells.size());
        put(s, index, picture, w, h);
        s.cells[id] = index;
        s.wanted.erase(id);
        s.dirty = true;
    }
    std::lock_guard lock(s.mutex);
    if (!s.dirty) return false;
    s.dirty = false;
    pixels.rgba = s.rgba;
    pixels.width = pixels.height = sheet_side;
    pixels.serial = ++s.serial;
    return true;
}

void avatar_sheet_uploaded(ImTextureID texture, unsigned serial) noexcept {
    auto &s = sheet();
    std::lock_guard lock(s.mutex);
    s.texture = texture;
    s.uploaded = serial;
}

void avatar_sheet_lost() noexcept {
    auto &s = sheet();
    std::lock_guard lock(s.mutex);
    s.texture = {};
    s.uploaded = 0;
    s.dirty = !s.cells.empty(); // uploaded again with the next frame
}

bool avatar_image(std::uint64_t steam_id, ImTextureID &texture, ImVec2 &uv0, ImVec2 &uv1) noexcept {
    auto &s = sheet();
    std::lock_guard lock(s.mutex);
    const auto found = s.cells.find(steam_id);
    if (found == s.cells.end() || !s.texture) return false; // (a cell filled since the last upload is blank for a frame)
    const float step = static_cast<float>(cell) / static_cast<float>(sheet_side);
    const float x = static_cast<float>(found->second % static_cast<int>(per_row)) * step, y = static_cast<float>(found->second / static_cast<int>(per_row)) * step;
    texture = s.texture;
    uv0 = ImVec2(x, y);
    uv1 = ImVec2(x + step, y + step);
    return true;
}
} // namespace dingosdk::overlay::detail
