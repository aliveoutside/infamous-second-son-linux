// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace iss::native {
// Tells src/pad.c when a graffiti stencil aims (graffiti.cpp). Before guest entry; 0 when the eboot
// does not match or ISS_GRAFFITI_AUTO=0, <0 on a failed write.
int InstallGraffiti(unsigned char* image, std::uint64_t image_size);
} // namespace iss::native
