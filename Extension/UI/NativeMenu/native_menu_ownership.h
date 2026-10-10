#pragma once
#include <map>

namespace dingosdk::multiplayer::menu_data {
// Register the private identity before following references. Native menus can
// share mutable labels and refer back to a model already being copied.
template<class Key,class Model> class UiCloneCache {
    std::map<Key,Model> copies_;
public:
    template<class Create,class Follow> Model clone(const Key& key,Create&& create,Follow&& follow) {
        if(const auto found=copies_.find(key);found!=copies_.end())return found->second;
        const auto model=create();copies_.emplace(key,model);follow(model);return model;
    }
};
} // namespace dingosdk::multiplayer::menu_data
