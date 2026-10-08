#include "skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include <array>
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
constexpr std::array<ModeInfo, 6> mode_info{{
    {"jam", "SPOT JAM", "Land lines inside the area. Every line adds to your score; highest total wins.", nullptr},
    {"1up", "1-UP", "Take turns. Beat the last score or take a strike; last one standing wins.", nullptr},
    {"meat", "HALL OF MEAT", "Bail as hard as you can. Speed, drops and tumbling score; most meat wins. Bone Cam included.", nullptr},
    {"race", "DEATHRACE", "Skate 3 style: everyone lines up at the start, races through every gate in order, first over the finish wins.", "ROUTE"},
    {"domination", "DOMINATION", "Take spots with your best line there. Every second you hold one scores.", "SPOTS"},
    {"graffiti", "GRAFFITI", "Grind it, gap it: what you skate takes your colour. A bigger line steals it. Most tags wins.", nullptr},
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
} // namespace

void modes_page(SkateMenu &menu, const Model &, const CallbacksV3 &callbacks) {
    auto &p = page();
    const auto m = modes_menu();
    ImGui::BeginChild("modes-page", ImVec2(0, page_body_height(menu)));
    const auto *current = find_mode(m.mode);
    if (!m.in_game) {
        begin_card(menu, "modes-new", "START A GAME", "Pick a Skate 3 online mode. Everyone in your lobby with game modes joins.");
        choice(menu, "modes-pick-a", p.pick, {"SPOT JAM", "1-UP", "HALL OF MEAT"});
        int second = p.pick - 3;
        if (choice(menu, "modes-pick-b", second, {"DEATHRACE", "DOMINATION", "GRAFFITI"})) p.pick = second + 3;
        p.pick = std::clamp(p.pick, 0, 5);
        note(mode_info[static_cast<std::size_t>(p.pick)].summary);
        if (primary_button(menu, std::format("SET UP {}", mode_info[static_cast<std::size_t>(p.pick)].name).c_str()))
            command(menu, callbacks, std::string("new ") + mode_info[static_cast<std::size_t>(p.pick)].key);
        end_card();
    } else if (m.leading && m.phase == 1) {
        begin_card(menu, "modes-setup", current ? current->name : "GAME", "You lead this game.");
        info(menu, "Players in", std::to_string(m.players));
        end_card();

        if (!m.placing.empty()) {
            begin_card(menu, "modes-placing", "PLACING", "Close this menu (Insert) and skate. The controls are on screen.");
            if (m.placing == "circle") note("Skate to the centre. D-pad Up/Down (PgUp/PgDn) sizes the circle; D-pad Right (Enter) sets it.");
            else note("D-pad Right (Enter) adds one where you stand, D-pad Left (Backspace) undoes, D-pad Down (End) finishes.");
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
        ImGui::SliderFloat("##circle-size", &p.circle, 5.0f, 150.0f, "%.0f m radius");
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
                note("Skate the course: D-pad Right drops the start, then each checkpoint; D-pad Down drops the finish.");
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
        if (m.mode == "1up") {
            setting(menu, callbacks, "Turn (seconds)", "##turn", p.turn, static_cast<int>(m.turn),
                    [](const char *id, int *v) { return ImGui::SliderInt(id, v, 10, 120); }, "turn");
            setting(menu, callbacks, "Strikes", "##strikes", p.strikes, m.strikes,
                    [](const char *id, int *v) { return ImGui::SliderInt(id, v, 1, 5); }, "strikes");
        } else {
            setting(menu, callbacks, "Time (seconds)", "##time", p.duration, static_cast<int>(m.duration),
                    [](const char *id, int *v) { return ImGui::SliderInt(id, v, 30, 1800); }, "time");
        }
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
        if (current) note(current->summary);
        if (m.leading) {
            if (primary_button(menu, "STOP GAME")) command(menu, callbacks, "stop");
        } else if (small_button(menu, "LEAVE GAME")) {
            command(menu, callbacks, "leave");
        }
        end_card();
    }

    begin_card(menu, "modes-bonecam", "BONE CAM", "Skate 2's X-ray bail view: your bones, and the ones you broke.");
    int when = m.bone_cam == "on" ? 1 : m.bone_cam == "off" ? 2 : 0;
    if (choice(menu, "modes-bonecam-when", when, {"HALL OF MEAT", "EVERY BAIL", "OFF"}))
        command(menu, callbacks, std::string("bonecam ") + (when == 1 ? "on" : when == 2 ? "off" : "meat"));
    note("The preview shows your skeleton. In a real bail the bones that take a hard hit crack or break.");
    if (small_button(menu, "PREVIEW BONE CAM")) command(menu, callbacks, "bonecam test");
    end_card();
    ImGui::EndChild();
}
} // namespace dingosdk::overlay::menu
