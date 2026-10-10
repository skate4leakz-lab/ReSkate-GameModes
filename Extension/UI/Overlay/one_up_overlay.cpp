#include "overlay_internal.h"
#include "multiplayer_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include "Extension/Throwdowns/one_up_runtime.h"
#include "Extension/UI/NativeMenu/one_up_menu.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dingosdk::overlay {
namespace {
using namespace multiplayer::one_up;
std::string score_text(double score) { char text[48]{}; std::snprintf(text, sizeof(text), "%.0f", score); return text; }
const char* phase_text(Phase phase) {
    switch (phase) {
    case Phase::lobby: return "READY UP";
    case Phase::countdown: return "GET READY";
    case Phase::playing: return "TURN SCORE";
    case Phase::settling: return "TIME UP";
    case Phase::feedback: return "TURN RESULT";
    case Phase::finished: return "LAST SKATER STANDING";
    case Phase::cancelled: return "MATCH ENDED";
    }
    return "1-UP";
}
void roster(const View& v, bool compact) {
    for (const auto& p : v.state.players) {
        ImGui::PushID(static_cast<int>(p.id));
        const auto colour = !p.eligible() ? skate_theme::danger : p.id == v.state.active ? skate_theme::bar : skate_theme::white;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colour), "%s%s", p.id == v.state.active ? "> " : "", v.name(p.id).c_str());
        ImGui::SameLine();
        for (unsigned letter = 0; letter < 3; ++letter) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(letter < p.penalties ? skate_theme::danger : skate_theme::grey_text), "%s", letter == 0 ? "[1]" : letter == 1 ? "[U]" : "[P]");
            if (letter < 2) ImGui::SameLine();
        }
        if (!compact) {
            ImGui::SameLine(); ImGui::TextDisabled("%s", !p.connected ? "Left" : p.penalties == 3 ? "Out" : v.state.phase == Phase::lobby ? (p.ready ? "Ready" : "Waiting") : "");
        }
        ImGui::PopID();
    }
}
}
namespace detail {
bool one_up_pending() {
    const auto v = multiplayer::one_up::view();
    if (!v.state.match || v.state.phase == Phase::lobby || multiplayer::native_one_up_setup_open()) return false;
    // A completed practice must not leave a permanent panel over free skating.
    static std::uint64_t ended_at{}, ended_match{};
    const bool ended=v.state.phase==Phase::cancelled || v.state.phase==Phase::finished;
    if(!ended) { ended_at=ended_match=0; return true; }
    if(ended_match!=v.state.match) { ended_match=v.state.match; ended_at=GetTickCount64(); }
    return GetTickCount64()-ended_at<5000;
}
void draw_one_up() {
    // The game owns all 1-Up gameplay counters, timers and countdowns.
    // Clearing the legacy feed also removes any panel drawn on the last frame.
    draw_one_up_hud({});
}

} // namespace detail
namespace menu::multiplayer_detail {
void one_up_card(SkateMenu& menu) {
    const auto v = multiplayer::one_up::view(); const auto& s = v.state;
    std::array<char, 65> unused{};
    const auto send = [&](std::string argument) { send_private(menu, "oneup", argument, unused, false); };
    begin_card(menu, "one-up", "1-UP", "Beat the previous turn total. Three penalties and you're out.");
    if (!s.match) {
        note("Spot Battle  /  Skate Jam  /  S.K.A.T.E. remain available in the game's Throwdowns menu.");
        static int seconds = 20;
        ImGui::SliderInt("Turn length", &seconds, 10, 120, "%d seconds");
        ImGui::BeginDisabled(!v.can_create);
        if (ImGui::Button("Place 1-Up here", ImVec2(-FLT_MIN, 0))) send("create " + std::to_string(seconds));
        ImGui::EndDisabled();
        if (!v.scoring_ready) note("Waiting for the game's scoring system...");
        for (const auto& offer : v.offers) {
            ImGui::PushID(static_cast<int>(offer.leader));
            const auto label = std::string(offer.started ? "Watch " : "Join ") + offer.name + "'s 1-Up";
            if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0))) send("join " + std::to_string(offer.leader) + " " + std::to_string(offer.match));
            ImGui::PopID();
        }
        note("Choose a starting spawn. Score tricks anywhere on the map.");
    } else {
        if (s.phase == Phase::finished) {
            const auto winner = v.name(s.winner) + " WINS!"; tag(menu, winner.c_str(), skate_theme::good);
            info(menu, "Final attempt", v.name(s.judged_player) + ": " + score_text(s.judged_score));
        } else tag(menu, phase_text(s.phase), skate_theme::bar);
        if (s.active) info(menu, "Up next", v.name(s.active));
        info(menu, "Target to beat", score_text(s.target)); info(menu, "Total this turn", score_text(s.best));
        info(menu, "Turn length", std::to_string(s.config.turn_ms / 1000) + " seconds");
        roster(v, false); note(s.notice.c_str());
        if (s.phase == Phase::lobby) {
            if (!v.participant) {
                if (ImGui::Button("Join rematch", ImVec2(-FLT_MIN, 0))) {
                    send("enroll");
                }
            } else if (ImGui::Button("Ready at the starting spot", ImVec2(-FLT_MIN, 0))) send("ready");
            if (s.leader == v.local && ImGui::Button("Start 1-Up", ImVec2(-FLT_MIN, 0))) send("start");
        }
        if (s.phase == Phase::finished && s.leader == v.local && ImGui::Button("Rematch", ImVec2(-FLT_MIN, 0))) send("rematch");
        if (s.phase == Phase::finished && s.leader != v.local) note("The host can open a rematch. Everyone chooses Ready again.");
        if (ImGui::Button("Exit 1-Up", ImVec2(-FLT_MIN, 0))) send("leave");
    }
    if (!v.status.empty()) note(v.status.c_str());
    end_card();
}
} // namespace menu::multiplayer_detail
} // namespace dingosdk::overlay
