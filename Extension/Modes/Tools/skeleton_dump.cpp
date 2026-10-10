// Development tool: finds an EBX asset in the installed game by part of its name and prints its
// fields, so the skater skeleton's bone names and parent indices can be read without the game
// running. Usage: skeleton_dump <game folder> <name part> [bundle part] [most assets, 3 by default]
#include "Engine/Vfs/game_bundles.h"
#include "Engine/Resource/ebx_document.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <span>
#include <string>

namespace fb = dingosdk::frostbite;
namespace ebx = dingosdk::frostbite::ebx;

namespace {
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
// The document being printed and its bytes, so a boxed value can be looked up in its side table.
const ebx::Document *current_document{};
std::span<const std::byte> current_bytes;
// A boxed value: its type code, its bytes, and any text the bytes point to (a string field keeps a
// 32-bit offset from where it is stored to the text).
void print_boxed(const ebx::BoxedReference &v, std::ostream &out) {
    out << "boxed type 0x" << std::hex << v.encodedType << std::dec;
    if (!current_document || v.dataOffset < 0) { out << '\n'; return; }
    const auto &records = current_document->boxedValues;
    const auto found = std::find_if(records.begin(), records.end(),
                                    [&](const ebx::BoxedValueRecord &r) { return r.offset == static_cast<std::uint64_t>(v.dataOffset); });
    if (found == records.end()) { out << " (no record)\n"; return; }
    const auto &raw = found->rawBytes;
    out << " [" << raw.size() << "]";
    static const char digits[] = "0123456789abcdef";
    out << ' ';
    for (std::size_t i = 0; i < std::min<std::size_t>(raw.size(), 48); ++i) {
        const auto b = static_cast<unsigned>(raw[i]);
        out << digits[b >> 4] << digits[b & 15];
    }
    const auto start = current_document->dataStart + found->offset;
    for (std::size_t i = 0; i + 4 <= raw.size(); i += 4) {
        std::uint32_t displacement{};
        std::memcpy(&displacement, raw.data() + i, 4);
        const auto at = start + i + displacement;
        if (displacement == 0 || displacement == 0xffffffffu || at >= current_document->dataEnd || at >= current_bytes.size()) continue;
        std::string text;
        for (auto p = at; p < current_bytes.size() && text.size() < 200; ++p) {
            const auto c = static_cast<char>(current_bytes[p]);
            if (c == 0) break;
            if (c < 0x20 || c > 0x7e) { text.clear(); break; }
            text += c;
        }
        if (text.size() >= 2) out << "  @" << i << " \"" << text << '"';
    }
    out << '\n';
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
        } else if constexpr (std::is_same_v<T, ebx::PointerReference>) {
            if (v.kind == ebx::PointerKind::external) out << "import " << v.index << '\n';
            else if (v.kind == ebx::PointerKind::null) out << "null\n";
            else out << "ptr " << v.index << '\n';
        }
        else if constexpr (std::is_same_v<T, ebx::BoxedReference>) print_boxed(v, out);
        else out << "(value)\n";
    }, value.data);
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 3) { std::cerr << "usage: skeleton_dump <game folder> <asset name part> [bundle name part]\n"; return 2; }
    const std::filesystem::path root = argv[1];
    const auto wanted = lower(argv[2]);
    const std::string bundle_filter = argc > 3 ? lower(argv[3]) : std::string();
    const int most = argc > 4 ? std::max(1, std::atoi(argv[4])) : 3;
    // `--list`: every EBX asset's name and bundle, without reading the assets.
    const bool list_only = wanted == "--list";
    // A fifth argument `all`: every object in the asset, numbered as `ptr N` names them.
    const bool every_object = argc > 5 && std::string(argv[5]) == "all";
    const dingosdk::vfs::GameData data(root);
    int found = 0;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root / L"Data" / L"Win32", error), end; it != end && !error; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != L".toc") continue;
        fb::TocDocument toc;
        const auto relative = std::filesystem::relative(it->path(), root / L"Data").generic_string();
        try { toc = data.read_toc(relative); } catch (const std::exception &e) {
            std::cerr << "skipped " << relative << ": " << e.what() << '\n';
            continue;
        }
        for (const auto &entry : toc.bundles) {
            if (!bundle_filter.empty() && lower(entry.name).find(bundle_filter) == std::string::npos) continue;
            std::optional<dingosdk::vfs::GameBundle> bundle;
            try { bundle = data.read_bundle(toc, entry.name); } catch (const std::exception &e) {
                std::cerr << "skipped bundle " << entry.name << ": " << e.what() << '\n';
                continue;
            }
            if (!bundle) continue;
            for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i) {
                const auto &asset = bundle->manifest.ebx[i];
                if (list_only) {
                    std::cout << asset.name << "\t" << entry.name << '\n';
                    continue;
                }
                if (lower(asset.name).find(wanted) == std::string::npos) continue;
                std::cout << "=== " << asset.name << "  (bundle " << entry.name << ", " << relative << ")\n";
                try {
                    const auto *payload = bundle->payload(fb::AssetKind::ebx, i);
                    if (!payload) { std::cout << "no payload\n"; continue; }
                    const auto bytes = data.read(*payload);
                    const auto document = ebx::read_document(bytes);
                    current_document = &document;
                    current_bytes = std::span<const std::byte>(bytes.data(), bytes.size());
                    std::cout << "root type: " << document.rootType << '\n';
                    if (const auto *rootRecord = document.root(); rootRecord && rootRecord->object) print_object(*rootRecord->object, 1, std::cout);
                    if (every_object)
                        for (std::size_t n = 0; n < document.instances.size(); ++n) {
                            const auto &record = document.instances[n];
                            if (!record.object) continue;
                            const auto d = record.object->descriptor;
                            const auto type = d >= 0 && static_cast<std::size_t>(d) < document.types.size() ? document.types[static_cast<std::size_t>(d)].name : std::string("?");
                            std::cout << "--- ptr " << n << "  " << type << '\n';
                            print_object(*record.object, 1, std::cout);
                        }
                } catch (const std::exception &e) {
                    std::cout << "read failed: " << e.what() << '\n';
                }
                if (++found >= most) return 0;
            }
        }
    }
    std::cout << (found ? "" : "not found\n");
    return found ? 0 : 1;
}
