// SPDX-License-Identifier: GPL-2.0-or-later
// ISS_PERF_MODE for the 1.07 eboot: the game's own quality settings, lowered at boot. Found in
// github.com/axylpup/shadPS4 (src/common/memory_patcher.cpp, ApplyPerfMode), who measured +13% (low)
// and +19% (office) FPS in the heaviest city view; the sites are compared byte for byte first.
//
// The game has a 21-row quality ladder at eboot+0xda08c0 (20 bytes a row: LOD scale of its 7 LOD
// distance bands, sun-shadow cascade offset, a distance nothing reads, a screen-size cull, random
// thinning). It climbs the ladder only when a frame overflows its GPU command memory; row 0 is
// full quality. A preset is written into row 0, so the game's own path runs it. Also forced: the
// low-resolution particle pass (the game steps it down on a GPU timer that means nothing here) and
// the streamer's high-resolution texture slots (the 4 nearest blocks) and, in "office", the ratio
// of ambient pedestrians.
//   low:    LOD 0.7, one sun-shadow cascade less, particles at half resolution, 2 high-res blocks.
//   office: LOD 0.5, one cascade less, particles at quarter resolution, no high-res blocks, half
//           the pedestrians.
#include "perf_mode.h"
#include "ultrawide.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace iss::native {
int InstallPerfMode(unsigned char *image, std::uint64_t size) {
    const char *env = std::getenv("ISS_PERF_MODE");
    const std::string mode = env ? env : "off";
    const bool low = mode == "low", office = mode == "office";
    if (!low && !office) return 0;
    struct Site { std::uint64_t at; std::vector<unsigned char> original, value; const char *what; };
    const std::vector<unsigned char> row0 = {0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24,
                                             0x74, 0x49, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::vector<unsigned char> row = row0;
    const float lod = low ? 0.7f : 0.5f;
    const std::int32_t cascades = -1;
    std::memcpy(row.data(), &lod, 4);
    std::memcpy(row.data() + 4, &cascades, 4);
    const unsigned char particle_level = low ? 1 : 2; // resolution 1.0 / 0.5 / 0.25 (table at 0xaaa918)
    const unsigned char hires_slots = low ? 2 : 0;
    std::vector<Site> sites = {
        {0xda08c0, row0, row, "quality ladder row 0"},
        // The je over the "set level" store becomes two nops, and the level it stores.
        {0x3ed6b0, {0x74, 0x0f}, {0x90, 0x90}, "particle level: always set"},
        {0x3ed6b8, {0x00, 0x00, 0x00, 0x00}, {particle_level, 0x00, 0x00, 0x00}, "particle level"},
        // mov r12d, 4 / cmp eax, 4: the 4 nearest blocks get the high-res textures.
        {0xfe9a2, {0x04}, {hires_slots}, "high-res texture slots (mov)"},
        {0xfe9b2, {0x04}, {hires_slots}, "high-res texture slots (cmp)"},
    };
    if (office) {
        // Pedestrians per class = level count x ratio of the world state: 1.0 -> 0.5.
        sites.push_back({0x40ca01, {0x00, 0x00, 0x80, 0x3f}, {0x00, 0x00, 0x00, 0x3f}, "pedestrian ratio 0"});
        sites.push_back({0x40c83f, {0x00, 0x00, 0x80, 0x3f}, {0x00, 0x00, 0x00, 0x3f}, "pedestrian ratio 1"});
    }
    for (const Site &s : sites)
        if (size < s.at + s.original.size() || std::memcmp(image + s.at, s.original.data(), s.original.size())) {
            std::fprintf(stderr, "PERF_MODE disabled: %s at eboot+0x%llx differs (1.07 eboot only)\n", s.what,
                         (unsigned long long)s.at);
            return 0;
        }
    for (const Site &s : sites)
        if (!WriteCode(image, s.at, s.value.data(), s.value.size())) return -1;
    std::fprintf(stderr, "PERF_MODE=%s: LOD scale %.1f, one sun-shadow cascade less, particles at %s resolution, %d high-res texture blocks%s\n",
                 mode.c_str(), double(lod), low ? "half" : "quarter", int(hires_slots), office ? ", half the pedestrians" : "");
    return 1;
}
} // namespace iss::native
