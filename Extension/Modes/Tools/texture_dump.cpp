// Development tool: decodes textures the installed game ships and writes them as uncompressed
// TGA files, to look at art before the overlay uses it (Deathrace's checkpoint gates).
// Usage: texture_dump <game folder> <toc> <bundle> <out folder> <side> <texture name>...
#include "Engine/Vfs/game_textures.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char **argv) {
    if (argc < 7) {
        std::fprintf(stderr, "usage: texture_dump <game folder> <toc> <bundle> <out folder> <side> <texture name>...\n");
        return 2;
    }
    dingosdk::vfs::GameTextures textures(argv[1]);
    const std::filesystem::path out = argv[4];
    std::filesystem::create_directories(out);
    const auto side = static_cast<std::uint32_t>(std::stoul(argv[5]));
    int failed = 0;
    for (int i = 6; i < argc; ++i) {
        try {
            const auto image = textures.read(argv[2], argv[3], argv[i], side);
            std::string file = argv[i];
            file = file.substr(file.find_last_of('/') + 1) + ".tga";
            std::ofstream tga(out / file, std::ios::binary);
            const unsigned char header[18]{0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                           static_cast<unsigned char>(image.width & 255), static_cast<unsigned char>(image.width >> 8),
                                           static_cast<unsigned char>(image.height & 255), static_cast<unsigned char>(image.height >> 8),
                                           32, 0x28};
            tga.write(reinterpret_cast<const char *>(header), sizeof(header));
            for (std::size_t p = 0; p < image.rgba.size(); p += 4) {
                const char bgra[4]{static_cast<char>(image.rgba[p + 2]), static_cast<char>(image.rgba[p + 1]),
                                   static_cast<char>(image.rgba[p]), static_cast<char>(image.rgba[p + 3])};
                tga.write(bgra, 4);
            }
            std::printf("%s: %ux%u -> %s\n", argv[i], image.width, image.height, file.c_str());
        } catch (const std::exception &e) {
            std::printf("%s: %s\n", argv[i], e.what());
            ++failed;
        }
    }
    return failed ? 1 : 0;
}
