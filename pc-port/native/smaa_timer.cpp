// SPDX-License-Identifier: GPL-2.0-or-later
// SMAA T2x without the GPU timer, for the 1.07 eboot. Each frame eboot+0x472810 decides whether the
// frame gets SMAA T2x (flag eboot+0xda07f9): on while the GPU frame time, read from two end of pipe
// timestamps (eboot+0x196ef0, units of 1/30000 s at 800 MHz), stays below 1000 (33.3 ms), back on below
// 950. Off, the callers (eboot+0x1929c0, +0x195840) draw another anti-aliasing instead: no SMAA passes,
// so FSR (fsr_aa) has nothing to run on and frame generation stops, and the projection jitter is no
// longer set (eboot+0x581a50) and stays at its last half-pixel offset. Here those timestamps follow the
// command processor and the guest's submissions, not a GPU (pm4_cmds.h GetGpuClock64), and two guest
// frames inside one measured frame already read 41.7 ms: the picture flipped between the two under load.
// The sbb that turns the comparison with 1000.0 into the condition becomes mov bl,0xff; the other
// conditions (eboot+0x7d3eb10+0xca0, the script and cutscene switches after it) are kept.
#include "smaa_timer.h"
#include "ultrawide.h"
#include <cstdio>
#include <cstring>

namespace iss::native {
int InstallSmaaTimer(unsigned char *image, std::uint64_t size) {
    // vucomiss xmm0, [rip -> 1000.0 at eboot+0xaaf024]; sbb bl, bl
    constexpr std::uint64_t at = 0x472894;
    const unsigned char original[] = {0xc5, 0xf8, 0x2e, 0x05, 0x88, 0xc7, 0x63, 0x00, 0x18, 0xdb};
    const unsigned char always[] = {0xb3, 0xff}; // mov bl, 0xff
    const float limit = 1000.0f;
    if (size < 0xaaf028 || std::memcmp(image + at, original, sizeof(original)) ||
        std::memcmp(image + 0xaaf024, &limit, sizeof(limit))) {
        std::fputs("SMAA_TIMER disabled: eboot+0x472894 differs (1.07 eboot only)\n", stderr);
        return 0;
    }
    if (!WriteCode(image, at + 8, always, sizeof(always))) return -1;
    std::fputs("SMAA_TIMER: SMAA T2x no longer turned off by the GPU timer\n", stderr);
    return 1;
}
} // namespace iss::native
