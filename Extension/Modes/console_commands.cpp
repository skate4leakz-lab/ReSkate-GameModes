#include "game_modes.h"
#include "Extension/Console/commands.h"

// The `mode` console commands: how a game mode is set up, started and left.
namespace dingosdk::console {
void register_mode_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
    };
    const Verb verbs[]{
        {"help", "How the game modes are played", nullptr},
        {"new", "Set up a game you lead", "jam|1up|meat|race|domination|graffiti|tag|skate"},
        {"tricks", "S.K.A.T.E.: the kinds of trick that can be set", "flips grabs grinds manuals|all"},
        {"spectate", "S.K.A.T.E.: watch whoever is up", "on|off"},
        {"grid", "Throwdowns: the game mode cards in a grid instead of one row (testing)", "on|off"},
        {"stance", "S.K.A.T.E.: flick diagrams for your stance", "regular|goofy"},
        {"diagram", "S.K.A.T.E.: show a trick's flick diagram for 10 seconds", "<trick, e.g. 360 Flip + Indy>"},
        {"circle", "Make the play area a circle around you", "[radius in metres]"},
        {"place", "Place the area, route or spots with the free camera (mouse and keyboard, or the pad)",
         "circle|corners|points|cancel|freecam on|off"},
        {"corner", "Mark a corner of the play area where you stand", "[undo|clear]"},
        {"point", "Place a checkpoint (Deathrace) or spot (Domination) where you stand", "[undo|clear]"},
        {"time", "How long a timed game lasts", "<seconds>"},
        {"turn", "1-Up: how long each turn is", "<seconds>"},
        {"strikes", "1-Up: misses before a player is out; S.K.A.T.E.: letters", "<1-5>"},
        {"radius", "How close counts as at a checkpoint or spot", "<metres>"},
        {"games", "Other players' games you can join", nullptr},
        {"join", "Join another player's game (the announced or nearest one, or the list's nth)", "[n]"},
        {"start", "Start the game you lead", nullptr},
        {"stop", "End the game you lead, for everyone", nullptr},
        {"leave", "Sit out the game you are in", nullptr},
        {"status", "The game you are in", nullptr},
        {"bonecam", "When the X-ray Bone Cam shows, or a preview of it", "[meat|on|off|test]"},
        {"bounce", "How much a ragdoll bounces off the ground in Hall of Meat (0 to 1)", "[0-1|off|meat|always]"},
        {"debug", "Diagnostics for building game modes", "states on|off"},
        {"results", "A sample results screen for 12 seconds", nullptr},
        {"tricklog", "Log the trick names skate. shows (for building S.K.A.T.E.)", "on|off"},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("mode ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            out(modes::command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
