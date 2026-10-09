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
constexpr std::array<ModeInfo, 8> mode_info{{
    {"jam", "SPOT JAM", "Land lines inside the area. Every line adds to your score; highest total wins.", nullptr},
    {"1up", "1-UP", "Take turns. Beat the last score or take a strike; last one standing wins.", nullptr},
    {"meat", "HALL OF MEAT", "Bail as hard as you can. Every bail scores its Meat; most meat wins. Your bones show through your skater.", nullptr},
    {"race", "DEATHRACE", "Skate 3 style: everyone lines up at the start, races through every gate in order, first over the finish wins.", "ROUTE"},
    {"domination", "DOMINATION", "Take spots with your best line there. Every second you hold one scores.", "SPOTS"},
    {"graffiti", "GRAFFITI", "Grind it, gap it: what you skate takes your colour. A bigger line steals it. Most tags wins.", nullptr},
    {"tag", "SKATE TAG", "One player is it and wears the crown: get close to tag someone else. No tag-backs. Least time spent it wins.", nullptr},
    {"skate", "S.K.A.T.E.", "Set a trick, everyone copies it or takes a letter. Spell S.K.A.T.E. and you're out; last one standing wins. You pick the tricks that count.", nullptr},
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
    if (!m.in_game) {
        begin_card(menu, "modes-new", "START A GAME", "Pick a Skate 3 online mode. Everyone in your lobby with game modes gets an invite.");
        choice(menu, "modes-pick-a", p.pick, {"SPOT JAM", "1-UP", "HALL OF MEAT"});
        int second = p.pick - 3;
        if (choice(menu, "modes-pick-b", second, {"DEATHRACE", "DOMINATION", "GRAFFITI"})) p.pick = second + 3;
        int third = p.pick - 6;
        if (choice(menu, "modes-pick-c", third, {"SKATE TAG", "S.K.A.T.E."})) p.pick = third + 6;
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
        ImGui::SliderFloat("##circle-size", &p.circle, 5.0f, m.mode == "tag" ? 1500.0f : 150.0f, "%.0f m radius", ImGuiSliderFlags_Logarithmic);
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
        if (m.mode == "tag")
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
