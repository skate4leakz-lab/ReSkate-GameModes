#pragma once
#include "mod_list.h"
#include <optional>

namespace dingosdk::mods {
// Native game data shipped inside ReSkate.dll, independent of Mods settings.
std::optional<Mod> one_up_core_assets(const std::filesystem::path& data_root);
}
