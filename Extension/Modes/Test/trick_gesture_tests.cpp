// S.K.A.T.E.'s flick diagrams: the gestures read from an installed game, and skate.'s trick names
// matched to them. Without a game folder (the first argument) there is nothing to read: skipped.
#include "Extension/Modes/trick_gestures.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace dingosdk::modes;

namespace {
void check(bool condition, const std::string &what) {
    if (!condition) throw std::runtime_error(what);
}
}

int main(int argc, char **argv) {
    if (argc < 2 || std::string(argv[1]).empty() || !std::filesystem::exists(std::filesystem::path(argv[1]) / "Skate.exe")) {
        std::cout << "Trick gestures: no game folder given, skipped.\n";
        return 0;
    }
    try {
        const auto count = load_trick_gestures(argv[1]);
        check(count >= 30, "fewer flip trick gestures than the game has: " + std::to_string(count));
        const auto kickflip = trick_gesture("Kickflip");
        check(kickflip.size() >= 2, "no kickflip");
        // Down (pulled back), then up and to the left for a regular stance.
        check(kickflip.front()[1] > 0.5f && kickflip.back()[1] < 0 && kickflip.back()[0] > 0.3f, "the kickflip's flick");
        const auto heelflip = trick_gesture("Heelflip");
        check(heelflip.size() >= 2 && heelflip.back()[0] < -0.3f, "the heelflip goes the other way");
        const auto ollie = trick_gesture("Ollie");
        check(ollie.size() >= 2 && ollie.front()[1] > 0.3f && ollie.back()[1] < -0.3f, "the ollie: back, then up");
        check(!trick_gesture("Nollie Kickflip").empty() && !trick_gesture("360 Flip").empty() && !trick_gesture("FS Pop Shove-it").empty() &&
                  !trick_gesture("Varial Heelflip").empty() && !trick_gesture("Impossible").empty(),
              "skate.'s names find their gestures");
        check(trick_gesture("Seatbelt").empty() && trick_gesture("BS 50-50 Grind").empty(), "grabs and grinds have no flick");
        std::cout << "Trick gestures: " << count << " read from the game; names matched.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
