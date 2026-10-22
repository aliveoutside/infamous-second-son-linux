// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace iss::native {
// ISS_RESOLUTION / ISS_HUD_ASPECT / ISS_FOV (ultrawide.cpp). Before guest entry; <0 on mismatch.
int InstallUltrawide(unsigned char* image, std::uint64_t image_size);
// FNV-1a 64 of code bytes (the identity checks) and a write into the image's code, page by page.
std::uint64_t CodeHash(const unsigned char* code, std::size_t size);
bool WriteCode(unsigned char* image, std::uint64_t at, const void* bytes, std::size_t size);
} // namespace iss::native
