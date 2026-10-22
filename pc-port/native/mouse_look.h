// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace iss::native {
// The mouse turns the player camera by an angle (mouse_look.cpp). Before guest entry; 0 when the
// eboot does not match (the mouse then drives the right stick), <0 on a failed write.
int InstallMouseLook(unsigned char* image, std::uint64_t image_size);
} // namespace iss::native
