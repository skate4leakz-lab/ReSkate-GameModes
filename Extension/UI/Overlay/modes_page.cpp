#include "skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <format>
#include <string>

// The GAME MODES page: sets up, starts and leaves the game modes (Extension/Modes) and picks when
// the Bone Cam shows. Presentation thread: it reads the game's snapshot (overlay::modes_menu) and
// queues `mode ...` console commands; it never touches the game.

namespace dingosdk::overlay {
namespace {
std::atomic<ModesMenuFeed> modes_menu_feed{};
}
void set_modes_menu_feed(ModesMenuFeed feed) noexcept { modes_menu_feed.store(feed); }
ModesMenu modes_menu() noexcept {
    try {
        if (const auto feed = modes_menu_feed.load()) return feed();
    } catch (...) {}
    return {};
}
} // namespace dingosdk::overlay

namespace dingosdk::overlay::menu {
namespace {
struct ModeInfo {
    const char *key, *name, *summary, *points; // points: what `mode point` places here, or null
};
constexpr std::array<ModeInfo, 9> mode_info{{
    {"jam", "SPOT JAM", "Land lines inside the area. Every line adds to your score; highest total wins.", nullptr},
    {"1up", "1-UP", "Take turns. Beat the last score or take a strike; last one standing wins.", nullptr},
    {"meat", "HALL OF MEAT", "Bail as hard as you can. Every bail scores its Meat; most meat wins. Your bones show through your skater.", nullptr},
    {"race", "DEATHRACE", "Skate 3 style: everyone lines up at the start, races through every gate in order, first over the finish wins.", "ROUTE"},
    {"domination", "DOMINATION", "Take spots with your best line there. Every second you hold one scores.", "SPOTS"},
    {"graffiti", "GRAFFITI", "Grind it, gap it: what you skate takes your colour. A bigger line steals it. Most tags wins.", nullptr},
    {"tag", "SKATE TAG", "One player is it and wears the crown: get close to tag someone else. No tag-backs. Least time spent it wins.", nullptr},
    {"skate", "S.K.A.T.E.", "Set a trick, everyone copies it or takes a letter. Spell S.K.A.T.E. and you're out; last one standing wins. You pick the tricks that count.", nullptr},
    {"infection", "INFECTION", "One player starts infected and turns into the Grim Reaper. Everyone they catch turns too and joins the hunt. Survive longest to win.", nullptr},
}};
struct Page {
    int pick{};
    int duration{-1}, turn{-1}, strikes{-1};
    float radius{-1};
    float circle{20}; // CIRCLE AROUND ME's radius
};
Page &page() {
    static auto *value = new Page;
    return *value;
}
const ModeInfo *find_mode(const std::string &key) {
    for (const auto &m : mode_info)
        if (key == m.key) return &m;
    return nullptr;
}
void command(SkateMenu &menu, const CallbacksV3 &callbacks, const std::string &text) { send_console(menu, callbacks, "mode " + text); }
// A row of small buttons on one line.
bool small_button(SkateMenu &menu, const char *label, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label, ImVec2(0, px(34)));
    ImGui::EndDisabled();
    (void)menu;
    return pressed;
}
// A slider that sends its value once it is let go.
template <class T, class Slider>
void setting(SkateMenu &menu, const CallbacksV3 &callbacks, const char *label, const char *id, T &edit, T live, Slider slider,
             const char *verb) {
    field(menu, label);
    if (edit < 0) edit = live;
    ImGui::SetNextItemWidth(-1);
    slider(id, &edit);
    if (ImGui::IsItemDeactivatedAfterEdit()) command(menu, callbacks, std::format("{} {}", verb, edit));
    if (!ImGui::IsItemActive() && !ImGui::IsItemDeactivated()) edit = live;
}
// A shiny 1st/2nd/3rd medal (or a plain grey disc further down) with the place in it.
void medal(ImDrawList *draw, ImVec2 centre, float radius, int place) {
    struct Metal {
        ImU32 dark, base, light;
    };
    static constexpr Metal metals[]{
        {IM_COL32(150, 98, 8, 255), IM_COL32(236, 178, 34, 255), IM_COL32(255, 236, 140, 255)},   // gold
        {IM_COL32(104, 110, 122, 255), IM_COL32(186, 192, 204, 255), IM_COL32(248, 250, 255, 255)}, // silver
        {IM_COL32(110, 56, 22, 255), IM_COL32(196, 112, 52, 255), IM_COL32(250, 186, 130, 255)},   // bronze
    };
    auto *font = ImGui::GetFont();
    const auto text = std::to_string(place);
    const float size = radius * 1.15f;
    if (place > 3) {
        draw->AddCircleFilled(centre, radius, skate_theme::tile_light, 32);
        const auto t = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
        draw->AddText(font, size, ImVec2(centre.x - t.x / 2, centre.y - t.y / 2), skate_theme::grey_text, text.c_str());
        return;
    }
    const auto &c = metals[place - 1];
    draw->AddCircleFilled(centre, radius + px(3), IM_COL32(0, 0, 0, 90), 32); // shadow
    draw->AddCircleFilled(centre, radius, c.dark, 32);
    draw->AddCircleFilled(ImVec2(centre.x, centre.y - radius * 0.06f), radius * 0.86f, c.base, 32);
    draw->AddCircleFilled(ImVec2(centre.x - radius * 0.18f, centre.y - radius * 0.24f), radius * 0.52f,
                          (c.light & 0x00ffffffu) | 0x70000000u, 32);
    draw->AddCircle(centre, radius * 0.86f, c.light, 32, px(1.5f));
    // A glint that sweeps across now and then.
    const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.5f + static_cast<float>(place) * 0.3f, 2.0f);
    if (phase < 1.0f) {
        const float x = centre.x - radius + phase * radius * 2;
        const float half = std::sqrt(std::max(0.0f, radius * radius - (x - centre.x) * (x - centre.x))) * 0.8f;
        draw->AddLine(ImVec2(x - half * 0.4f, centre.y + half), ImVec2(x + half * 0.4f, centre.y - half), IM_COL32(255, 255, 255, 150), px(3));
    }
    const auto t = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    const ImVec2 at(centre.x - t.x / 2, centre.y - t.y / 2);
    draw->AddText(font, size, ImVec2(at.x + px(1), at.y + px(1)), c.dark, text.c_str());
    draw->AddText(font, size, at, skate_theme::white, text.c_str());
}
std::string ago(std::uint32_t seconds) {
    if (seconds < 60) return "just now";
    if (seconds < 3600) return std::format("{}m ago", seconds / 60);
    return std::format("{}h ago", seconds / 3600);
}
// The lobby leaderboard: medals, points bars, streaks and what each player wins most.
void leaderboard(SkateMenu &menu, const ModesMenu &m) {
    const auto subtitle = m.lobby_games == 0 ? std::string("Every game of 2+ players here counts. 1st 3 points, 2nd 2, 3rd 1.")
                          : std::format("{} game{} played  -  most played: {}", m.lobby_games, m.lobby_games == 1 ? "" : "s", m.lobby_top_mode);
    begin_card(menu, "modes-leaders", "LOBBY LEADERBOARD", subtitle.c_str());
    if (m.leaders.empty()) {
        note("Nobody is on the board yet. Finish a game with someone and the standings show up here. It resets when you leave the lobby.");
        end_card();
        return;
    }
    auto *draw = ImGui::GetWindowDrawList();
    auto *font = ImGui::GetFont();
    const float base = ImGui::GetFontSize();
    const float width = ImGui::GetContentRegionAvail().x;
    const int top_points = std::max(1, m.leaders.front().points);
    const std::size_t shown = std::min<std::size_t>(m.leaders.size(), 8);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto &r = m.leaders[i];
        const int place = static_cast<int>(i) + 1;
        const float height = px(i == 0 ? 76.0f : 62.0f);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + height);
        // Row: the leader glows gold, the local player blue.
        const ImU32 fill = i == 0 ? IM_COL32(64, 48, 10, 235) : r.you ? skate_theme::official_tile : skate_theme::tile_grey;
        draw->AddRectFilled(a, b, fill, px(8));
        if (i == 0) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.5f);
            draw->AddRect(a, b, IM_COL32(255, 200, 60, static_cast<int>(120 + 100 * pulse)), px(8), 0, px(2));
        } else if (r.you) {
            draw->AddRect(a, b, skate_theme::official, px(8), 0, px(1.5f));
        }
        const float radius = height * 0.32f;
        medal(draw, ImVec2(a.x + px(14) + radius, a.y + height / 2), radius, place);
        const float left = a.x + px(28) + radius * 2;
        const float right_space = px(130);
        // Name (with LEADER / YOU / streak pills), then the stats line under it.
        const float name_size = base * (i == 0 ? 1.35f : 1.15f);
        auto name = r.name.empty() ? std::string("Player") : r.name;
        while (name.size() > 4 &&
               font->CalcTextSizeA(name_size, FLT_MAX, 0, name.c_str()).x > b.x - right_space - left - px(150))
            name = name.substr(0, name.size() - 4) + "...";
        const float name_y = a.y + height / 2 - name_size + px(2);
        draw->AddText(font, name_size, ImVec2(left, name_y), i == 0 ? IM_COL32(255, 226, 120, 255) : skate_theme::white, name.c_str());
        float tag_x = left + font->CalcTextSizeA(name_size, FLT_MAX, 0, name.c_str()).x + px(10);
        const auto pill = [&](const std::string &text, ImU32 back, ImU32 fore) {
            const float s = base * 0.72f;
            const auto t = font->CalcTextSizeA(s, FLT_MAX, 0, text.c_str());
            if (tag_x + t.x + px(12) > b.x - right_space) return;
            const ImVec2 p(tag_x, name_y + (name_size - t.y) / 2 - px(1));
            draw->AddRectFilled(p, ImVec2(p.x + t.x + px(12), p.y + t.y + px(4)), back, px(10));
            draw->AddText(font, s, ImVec2(p.x + px(6), p.y + px(2)), fore, text.c_str());
            tag_x += t.x + px(18);
        };
        if (i == 0) pill("LEADER", IM_COL32(255, 196, 40, 255), skate_theme::black);
        if (r.you) pill("YOU", skate_theme::official, skate_theme::white);
        if (r.streak >= 2) pill(std::format("{} IN A ROW", r.streak), IM_COL32(255, 92, 30, 255), skate_theme::white);
        auto stats = std::format("{} win{}  |  {} podium{}  |  {} game{}", r.wins, r.wins == 1 ? "" : "s", r.podiums,
                                 r.podiums == 1 ? "" : "s", r.games, r.games == 1 ? "" : "s");
        if (!r.best_mode.empty()) stats += "  |  best at " + r.best_mode;
        if (r.best_streak >= 2 && r.streak < r.best_streak) stats += std::format("  |  best streak {}", r.best_streak);
        draw->PushClipRect(ImVec2(left, a.y), ImVec2(b.x - right_space, b.y), true);
        draw->AddText(font, base * 0.82f, ImVec2(left, a.y + height / 2 + px(4)), skate_theme::grey_text, stats.c_str());
        draw->PopClipRect();
        // Points, big on the right, over a bar of how close they are to the top.
        const auto points = std::to_string(r.points);
        const float big = base * (i == 0 ? 1.9f : 1.55f), small = base * 0.7f;
        const auto pt = font->CalcTextSizeA(big, FLT_MAX, 0, points.c_str());
        const float edge = b.x - px(16);
        const float top = a.y + (height - pt.y - px(14)) / 2;
        draw->AddText(font, big, ImVec2(edge - pt.x - px(30), top), skate_theme::white, points.c_str());
        draw->AddText(font, small, ImVec2(edge - px(26), top + pt.y - small - px(3)), skate_theme::grey_text, "PTS");
        const ImVec2 bar_a(edge - px(100), b.y - px(14)), bar_b(edge, b.y - px(9));
        draw->AddRectFilled(bar_a, bar_b, IM_COL32(0, 0, 0, 120), px(3));
        const float share = std::clamp(static_cast<float>(r.points) / static_cast<float>(top_points), 0.0f, 1.0f);
        const ImU32 bar_colour = place == 1 ? IM_COL32(255, 196, 40, 255) : place == 2 ? IM_COL32(200, 206, 218, 255)
                               : place == 3 ? IM_COL32(214, 124, 60, 255) : skate_theme::blue;
        if (share > 0) draw->AddRectFilled(bar_a, ImVec2(bar_a.x + (bar_b.x - bar_a.x) * share, bar_b.y), bar_colour, px(3));
        ImGui::Dummy(ImVec2(width, height + px(4)));
    }
    if (m.leaders.size() > shown) note(std::format("+ {} more", m.leaders.size() - shown).c_str());
    end_card();

    if (m.recent.empty()) return;
    begin_card(menu, "modes-recent", "RECENT GAMES", "The last games finished in this lobby.");
    draw = ImGui::GetWindowDrawList();
    const float row_width = ImGui::GetContentRegionAvail().x;
    for (const auto &g : m.recent) {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float height = px(48);
        draw->AddRectFilled(a, ImVec2(a.x + row_width, a.y + height), skate_theme::tile_grey, px(6));
        draw->AddRectFilled(a, ImVec2(a.x + px(5), a.y + height), IM_COL32(255, 196, 40, 255), px(6), ImDrawFlags_RoundCornersLeft);
        const float s = base * 0.8f;
        draw->AddText(font, s, ImVec2(a.x + px(16), a.y + px(5)), skate_theme::official_text, g.mode.c_str());
        const auto when = std::format("{} players  -  {}", g.players, ago(g.ago_s));
        const auto wt = font->CalcTextSizeA(s, FLT_MAX, 0, when.c_str());
        draw->AddText(font, s, ImVec2(a.x + row_width - wt.x - px(12), a.y + px(5)), skate_theme::grey_text, when.c_str());
        const auto line = std::format("{} won  -  {} 2nd", g.winner, g.second);
        draw->PushClipRect(a, ImVec2(a.x + row_width - px(8), a.y + height), true);
        draw->AddText(font, base, ImVec2(a.x + px(16), a.y + px(7) + s), skate_theme::white, line.c_str());
        draw->PopClipRect();
        ImGui::Dummy(ImVec2(row_width, height + px(3)));
    }
    end_card();
}
} // namespace

void modes_page(SkateMenu &menu, const Model &, const CallbacksV3 &callbacks) {
    auto &p = page();
    const auto m = modes_menu();
    ImGui::BeginChild("modes-page", ImVec2(0, page_body_height(menu)));
    const auto *current = find_mode(m.mode);
    // Other players' games, like the throwdown list: what is on, who hosts it, and a way in.
    if (!m.offers.empty()) {
        begin_card(menu, "modes-open", "OPEN GAMES", "Other players' games. Joining puts you at its start.");
        for (const auto &offer : m.offers) {
            ImGui::PushID(static_cast<int>(offer.id & 0x7fffffff));
            info(menu, offer.mode.c_str(), offer.host + "  -  " + offer.detail);
            if (primary_button(menu, offer.open ? "JOIN" : "IN PROGRESS", offer.open && !(m.in_game && m.phase != 4)))
                command(menu, callbacks, std::format("join {}", offer.id));
            ImGui::PopID();
        }
        end_card();
    }
    // Who in the lobby can play: game modes only reach players who have them, in the same version.
    if (m.in_session) {
        begin_card(menu, "modes-lobby", "PLAYERS HERE", "Only players with ReSkate game modes (the same version) see and join games.");
        const auto list = [](const std::vector<std::string> &names) {
            std::string text;
            for (const auto &name : names) text += (text.empty() ? "" : ", ") + name;
            return text;
        };
        info(menu, "Can play", m.lobby_modded.empty() ? std::string("nobody yet") : list(m.lobby_modded));
        if (!m.lobby_outdated.empty()) {
            info(menu, "Need to update", list(m.lobby_outdated));
            warn("Players on another version: close skate. and start it again with ReSkateLauncher.exe to update (whoever is older).");
        }
        if (m.lobby_without > 0) info(menu, "No game modes", std::to_string(m.lobby_without) + (m.lobby_without == 1 ? " player" : " players"));
        end_card();
    }
    if (m.in_session) leaderboard(menu, m);
    if (!m.in_game) {
        begin_card(menu, "modes-new", "START A GAME", "Pick a Skate 3 online mode. Everyone in your lobby with game modes gets an invite.");
        choice(menu, "modes-pick-a", p.pick, {"SPOT JAM", "1-UP", "HALL OF MEAT"});
        int second = p.pick - 3;
        if (choice(menu, "modes-pick-b", second, {"DEATHRACE", "DOMINATION", "GRAFFITI"})) p.pick = second + 3;
        int third = p.pick - 6;
        if (choice(menu, "modes-pick-c", third, {"SKATE TAG", "S.K.A.T.E.", "INFECTION"})) p.pick = third + 6;
        p.pick = std::clamp(p.pick, 0, static_cast<int>(mode_info.size()) - 1);
        note(mode_info[static_cast<std::size_t>(p.pick)].summary);
        if (primary_button(menu, std::format("SET UP {}", mode_info[static_cast<std::size_t>(p.pick)].name).c_str()))
            command(menu, callbacks, std::string("new ") + mode_info[static_cast<std::size_t>(p.pick)].key);
        end_card();
    } else if (m.leading && m.phase == 1) {
        begin_card(menu, "modes-setup", current ? current->name : "GAME", "You lead this game.");
        info(menu, "Players in", std::to_string(m.players));
        end_card();

        if (!m.placing.empty()) {
            begin_card(menu, "modes-placing", "PLACING", "Close this menu (Insert): the free camera flies and the controls are on screen.");
            note("Fly with WASD, Q/E and the right mouse button (or the sticks and triggers). Aim the reticle at the ground.");
            if (m.placing == "circle") note("Mouse wheel sizes the circle; left click (Enter) sets it; Backspace cancels.");
            else if (m.mode == "race") note("Left click drops the start, then each checkpoint; F drops the finish. The wheel turns a gate, Ctrl + wheel (or [ ]) widens it. Backspace undoes.");
            else note("Left click (Enter) drops one where you aim, Backspace undoes, F (End) finishes.");
            if (small_button(menu, "STOP PLACING")) command(menu, callbacks, "place cancel");
            end_card();
        }

        if (m.mode != "race") {
        begin_card(menu, "modes-area", "PLAY AREA", "Like a skate. jam: a circle around your spot. Without one, the whole map counts.");
        info(menu, "Area", m.area_radius > 0 ? std::format("circle, {:.0f} m across", m.area_radius * 2)
                           : m.corners >= 3 ? std::format("{} corners", m.corners)
                           : "none (the whole map)");
        if (primary_button(menu, "PLACE CIRCLE")) command(menu, callbacks, "place circle");
        if (small_button(menu, "CIRCLE AROUND ME")) command(menu, callbacks, std::format("circle {:.0f}", p.circle));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(200));
        // Skate Tag can spread over a whole district: up to 1.5 km out.
        ImGui::SliderFloat("##circle-size", &p.circle, 5.0f, m.mode == "tag" || m.mode == "infection" ? 1500.0f : 150.0f, "%.0f m radius",
                           ImGuiSliderFlags_Logarithmic);
        if (small_button(menu, "CUSTOM SHAPE")) command(menu, callbacks, "place corners");
        ImGui::SameLine();
        if (small_button(menu, "CLEAR##area", m.corners > 0)) command(menu, callbacks, "corner clear");
        end_card();
        }

        if (current && current->points) {
            begin_card(menu, "modes-points", current->points,
                       m.mode == "race" ? "In the order they are raced." : nullptr);
            if (m.mode == "race") {
                info(menu, "Route", m.points == 0 ? std::string("not set")
                                    : m.points == 1 ? std::string("start only")
                                    : std::format("start, {} checkpoint{}, finish", m.points - 2, m.points == 3 ? "" : "s"));
                note("Fly the free camera along the course: left click drops the start and each checkpoint, F the finish. Turn a gate with the wheel, widen it with Ctrl + wheel.");
                if (primary_button(menu, "PLACE ROUTE")) command(menu, callbacks, "place points");
            } else {
                info(menu, "Placed", std::to_string(m.points));
                if (primary_button(menu, std::format("PLACE {}", current->points).c_str())) command(menu, callbacks, "place points");
            }
            if (small_button(menu, "UNDO##point", m.points > 0)) command(menu, callbacks, "point undo");
            ImGui::SameLine();
            if (small_button(menu, "CLEAR##point", m.points > 0)) command(menu, callbacks, "point clear");
            end_card();
        }

        begin_card(menu, "modes-rules", "RULES");
        if (m.mode == "skate") {
            // The kinds of trick that may be set: at least one stays on.
            field(menu, "Tricks that count");
            struct Kind {
                unsigned bit;
                const char *label, *word;
            };
            constexpr Kind kinds[]{{1, "FLIP TRICKS", "flips"}, {2, "GRABS", "grabs"}, {4, "GRINDS & SLIDES", "grinds"}, {8, "MANUALS", "manuals"}};
            for (const auto &kind : kinds) {
                bool on = (m.trick_kinds & kind.bit) != 0;
                if (ImGui::Checkbox(kind.label, &on)) {
                    const unsigned next = on ? m.trick_kinds | kind.bit : m.trick_kinds & ~kind.bit;
                    if (next) {
                        std::string words;
                        for (const auto &k : kinds)
                            if (next & k.bit) words += std::string(" ") + k.word;
                        command(menu, callbacks, "tricks" + words);
                    }
                }
                ImGui::SameLine();
            }
            ImGui::NewLine();
            note("A combo counts as every kind in it: a kickflip into a grab needs both FLIP TRICKS and GRABS.");
            setting(menu, callbacks, "Letters (5 spells S.K.A.T.E.)", "##letters", p.strikes, m.strikes,
                    [](const char *id, int *v) { return ImGui::SliderInt(id, v, 1, 5); }, "strikes");
            setting(menu, callbacks, "Turn (seconds)", "##turn", p.turn, static_cast<int>(m.turn),
                    [](const char *id, int *v) { return ImGui::SliderInt(id, v, 10, 120); }, "turn");
        } else if (m.mode == "1up") {
            setting(menu, callbacks, "Turn (seconds)", "##turn", p.turn, static_cast<int>(m.turn),
                    [](const char *id, int *v) { return ImGui::SliderInt(id, v, 10, 120); }, "turn");
            setting(menu, callbacks, "Strikes", "##strikes", p.strikes, m.strikes,
                    [](const char *id, int *v) { return ImGui::SliderInt(id, v, 1, 5); }, "strikes");
        } else {
            setting(menu, callbacks, "Time (seconds)", "##time", p.duration, static_cast<int>(m.duration),
                    [](const char *id, int *v) { return ImGui::SliderInt(id, v, 30, 1800); }, "time");
        }
        if (m.mode == "tag" || m.mode == "infection")
            setting(menu, callbacks, "Tag reach (metres)", "##reach", p.radius, m.radius,
                    [](const char *id, float *v) { return ImGui::SliderFloat(id, v, 1.5f, 8.0f, "%.1f"); }, "radius");
        if (current && current->points)
            setting(menu, callbacks, "Reach (metres)", "##radius", p.radius, m.radius,
                    [](const char *id, float *v) { return ImGui::SliderFloat(id, v, 2.0f, 30.0f, "%.0f"); }, "radius");
        end_card();

        if (!m.missing.empty()) warn(m.missing.c_str());
        if (primary_button(menu, "START GAME", m.missing.empty())) command(menu, callbacks, "start");
        if (small_button(menu, "CANCEL GAME")) command(menu, callbacks, "stop");
    } else {
        static constexpr const char *phases[]{"", "Setting up", "Starting", "Playing", "Results"};
        begin_card(menu, "modes-running", current ? current->name : "GAME", m.leading ? "Your game." : "You're in this game.");
        info(menu, "Now", phases[std::clamp(m.phase, 0, 4)]);
        info(menu, "Players", std::to_string(m.players));
        if (m.mode == "skate") {
            field(menu, "Camera on other turns");
            int follow = m.spectate ? 0 : 1;
            if (choice(menu, "modes-spectate", follow, {"WATCH WHO'S UP", "STAY ON ME"})) command(menu, callbacks, follow == 0 ? "spectate on" : "spectate off");
            field(menu, "Flick diagrams for");
            int stance = m.goofy ? 1 : 0;
            if (choice(menu, "modes-stance", stance, {"REGULAR", "GOOFY"})) command(menu, callbacks, stance == 1 ? "stance goofy" : "stance regular");
        }
        if (current) note(current->summary);
        if (m.leading) {
            if (primary_button(menu, "STOP GAME")) command(menu, callbacks, "stop");
        } else if (small_button(menu, "LEAVE GAME")) {
            command(menu, callbacks, "leave");
        }
        end_card();
    }

    if (m.official_meat) {
        // ReSkate's own Hall of Meat: the skeleton through your skater, the Meat card, slow motion on a break.
        begin_card(menu, "modes-bonecam", "HALL OF MEAT", "Your bones show through your skater when you bail, with the bail's Meat card.");
        int every = m.meat_every_bail ? 1 : 0;
        if (choice(menu, "modes-meat-when", every, {"HALL OF MEAT GAMES", "EVERY BAIL"}))
            command(menu, callbacks, every == 1 ? "bonecam on" : "bonecam meat");
        note("Hall of Meat games always show it. EVERY BAIL shows it in free skate too (the same switch as Custom Stuff > Player > Hall of Meat).");
        end_card();
        ImGui::EndChild();
        return;
    }
    begin_card(menu, "modes-bonecam", "BONE CAM", "Skate 2's X-ray bail view: your bones, and the ones you broke.");
    int when = m.bone_cam == "on" ? 1 : m.bone_cam == "off" ? 2 : 0;
    if (choice(menu, "modes-bonecam-when", when, {"HALL OF MEAT", "EVERY BAIL", "OFF"}))
        command(menu, callbacks, std::string("bonecam ") + (when == 1 ? "on" : when == 2 ? "off" : "meat"));
    field(menu, "Concussion ringing");
    int ringing = m.bone_cam_ringing ? 0 : 1;
    if (choice(menu, "modes-bonecam-ring", ringing, {"ON", "MUTED"})) command(menu, callbacks, ringing == 0 ? "bonecam ring on" : "bonecam ring off");
    note("The preview shows your skeleton. In a real bail the bones that take a hard hit crack or break.");
    note("Skeleton: BodyParts3D, (c) The Database Center for Life Science, licensed under CC Attribution 4.0 International.");
    if (small_button(menu, "PREVIEW BONE CAM")) command(menu, callbacks, "bonecam test");
    end_card();
    ImGui::EndChild();
}
} // namespace dingosdk::overlay::menu
