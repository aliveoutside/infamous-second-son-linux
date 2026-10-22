// SPDX-License-Identifier: GPL-2.0-or-later
// Linux guest TLS: the guest reads its thread control block through fs:, which on Linux belongs
// to the host's libc. Every `fs:[disp32]` access of the executable image (REX.W, ModRM+SIB with
// no base/index: the only form the eboot and its modules use) is rewritten to gs: (same length:
// the prefix byte 0x64 -> 0x65), and each guest thread points its GS base at its TCB
// (runtime_tls_attach). Done once, before any guest thread runs. A byte scan, not a disassembly:
// a 0x64 inside a displacement must not be mistaken for a prefix, hence the strict shape.
#include "../runtime.h"
#include <stdio.h>

int runtime_patch_tcb_accesses(unsigned char *code, uint64_t size) {
    uint64_t patched = 0;
    for (uint64_t i = 0; i + 9 <= size; ++i) {
        if (code[i] != 0x64 || (code[i + 1] != 0x48 && code[i + 1] != 0x4c)) continue;
        const unsigned char modrm = code[i + 3];
        if ((modrm & 0xc7) != 0x04 || code[i + 4] != 0x25) continue;
        code[i] = 0x65;
        ++patched;
        i += 8;
    }
    fprintf(stderr, "TCB_PATCH fs->gs instructions=%llu\n", (unsigned long long)patched);
    return (int)patched;
}
