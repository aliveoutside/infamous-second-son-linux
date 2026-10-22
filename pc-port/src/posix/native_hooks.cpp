// SPDX-License-Identifier: GPL-2.0-or-later
// Linux: engine patches installed into the loaded eboot before module initializers and guest entry.
// Only native/ultrawide.cpp (ISS_RESOLUTION, ISS_FOV, ISS_HUD_ASPECT) is built; the other native/*
// replacements (ISS_NATIVE_*, ISS_SPIN_PROBE, ISS_FEEDBACK_IDENTITY experiments) are not ported.
#include "../runtime.h"
#include "../../native/ultrawide.h"
extern "C" int native_install_hooks(unsigned char *image, uint64_t size) {
    return iss::native::InstallUltrawide(image, size);
}
