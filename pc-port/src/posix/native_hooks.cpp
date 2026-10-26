// SPDX-License-Identifier: GPL-2.0-or-later
// Linux: engine patches installed into the loaded eboot before module initializers and guest entry.
// Only native/ultrawide.cpp (ISS_RESOLUTION, ISS_FOV, ISS_HUD_ASPECT), native/mouse_look.cpp,
// native/graffiti.cpp, native/perf_mode.cpp and native/smaa_timer.cpp are built; the other native/* replacements
// (experiments) are not ported.
#include "../runtime.h"
#include "../../native/graffiti.h"
#include "../../native/mouse_look.h"
#include "../../native/perf_mode.h"
#include "../../native/smaa_timer.h"
#include "../../native/ultrawide.h"
extern "C" int native_install_hooks(unsigned char *image, uint64_t size) {
    const int ultrawide = iss::native::InstallUltrawide(image, size);
    if (ultrawide < 0) return ultrawide;
    const int look = iss::native::InstallMouseLook(image, size);
    if (look < 0) return look;
    const int graffiti = iss::native::InstallGraffiti(image, size);
    if (graffiti < 0) return graffiti;
    const int perf = iss::native::InstallPerfMode(image, size);
    if (perf < 0) return perf;
    const int smaa = iss::native::InstallSmaaTimer(image, size);
    return smaa < 0 ? smaa : ultrawide + look + graffiti + perf + smaa;
}
