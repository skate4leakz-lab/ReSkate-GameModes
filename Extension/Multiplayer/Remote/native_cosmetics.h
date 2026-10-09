#pragma once
#include "native_skater.h"
namespace dingosdk::multiplayer {
std::optional<Appearance> capture_cosmetics(std::uintptr_t base, const NativeFrame &, std::string &detail);
// Only called for the owned remote actor on the verified client update thread.
void apply_cosmetic_recipe(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t local_entity,
                           const CosmeticRecipe &);
// Game Modes' Infection: `recipe` with a costume this PC has (found in the local catalog by name
// parts, case-insensitive: "costume" and `name`) in the slot of the skater template it fits, the
// local skater's template telling which. False (recipe unchanged) when the costume or a slot for
// it is not found. Client update thread.
bool dress_in_costume(std::uintptr_t base, std::uintptr_t local_entity, CosmeticRecipe &recipe, std::string_view name);
} // namespace dingosdk::multiplayer
