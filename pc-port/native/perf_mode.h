// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace iss::native {
// ISS_PERF_MODE=low|office (perf_mode.cpp): the game's own lower quality settings. Before guest
// entry; 0 when off or the eboot does not match, <0 on a failed write.
int InstallPerfMode(unsigned char* image, std::uint64_t image_size);
} // namespace iss::native
