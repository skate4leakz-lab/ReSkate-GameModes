// Development tool: finds an EBX asset in the installed game by part of its name and prints its
// fields, so the skater skeleton's bone names and parent indices can be read without the game
// running. Usage: skeleton_dump <game folder> <name part> [bundle part]
#include "Engine/Vfs/game_bundles.h"
#include "Engine/Resource/ebx_document.h"
#include <algorithm>
#include <iostream>
#include <string>

namespace fb = dingosdk::frostbite;
namespace ebx = dingosdk::frostbite::ebx;

namespace {
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
void print(const ebx::Value &value, int depth, std::ostream &out);
void print_object(const ebx::Object &object, int depth, std::ostream &out) {
    for (const auto &field : object.fields) {
        out << std::string(depth * 2, ' ') << field.name << ": ";
        print(field.value, depth + 1, out);
    }
}
void print(const ebx::Value &value, int depth, std::ostream &out) {
    std::visit([&](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) out << "(none)\n";
        else if constexpr (std::is_same_v<T, bool>) out << (v ? "true" : "false") << '\n';
        else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t> || std::is_same_v<T, double>) out << v << '\n';
        else if constexpr (std::is_same_v<T, std::string>) out << '"' << v << "\"\n";
        else if constexpr (std::is_same_v<T, std::shared_ptr<ebx::Object>>) {
            out << "{\n";
            if (v && depth < 8) print_object(*v, depth, out);
            out << std::string((depth - 1) * 2, ' ') << "}\n";
        } else if constexpr (std::is_same_v<T, ebx::Value::Array>) {
            out << "[" << v.size() << "]\n";
            std::size_t index = 0;
            for (const auto &item : v) {
                if (index >= 8000) { out << std::string(depth * 2, ' ') << "...\n"; break; }
                out << std::string(depth * 2, ' ') << '#' << index++ << ' ';
                print(item, depth + 1, out);
            }
        } else if constexpr (std::is_same_v<T, ebx::PointerReference>) out << "ptr " << v.index << '\n';
        else out << "(value)\n";
    }, value.data);
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 3) { std::cerr << "usage: skeleton_dump <game folder> <asset name part> [bundle name part]\n"; return 2; }
    const std::filesystem::path root = argv[1];
    const auto wanted = lower(argv[2]);
    const std::string bundle_filter = argc > 3 ? lower(argv[3]) : std::string();
    const dingosdk::vfs::GameData data(root);
    int found = 0;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root / L"Data" / L"Win32", error), end; it != end && !error; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != L".toc") continue;
        fb::TocDocument toc;
        const auto relative = std::filesystem::relative(it->path(), root / L"Data").generic_string();
        try { toc = data.read_toc(relative); } catch (...) { continue; }
        for (const auto &entry : toc.bundles) {
            if (!bundle_filter.empty() && lower(entry.name).find(bundle_filter) == std::string::npos) continue;
            std::optional<dingosdk::vfs::GameBundle> bundle;
            try { bundle = data.read_bundle(toc, entry.name); } catch (...) { continue; }
            if (!bundle) continue;
            for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i) {
                const auto &asset = bundle->manifest.ebx[i];
                if (lower(asset.name).find(wanted) == std::string::npos) continue;
                std::cout << "=== " << asset.name << "  (bundle " << entry.name << ", " << relative << ")\n";
                try {
                    const auto *payload = bundle->payload(fb::AssetKind::ebx, i);
                    if (!payload) { std::cout << "no payload\n"; continue; }
                    const auto bytes = data.read(*payload);
                    const auto document = ebx::read_document(bytes);
                    std::cout << "root type: " << document.rootType << '\n';
                    if (const auto *rootRecord = document.root(); rootRecord && rootRecord->object) print_object(*rootRecord->object, 1, std::cout);
                } catch (const std::exception &e) {
                    std::cout << "read failed: " << e.what() << '\n';
                }
                if (++found >= 3) return 0;
            }
        }
    }
    std::cout << (found ? "" : "not found\n");
    return found ? 0 : 1;
}
