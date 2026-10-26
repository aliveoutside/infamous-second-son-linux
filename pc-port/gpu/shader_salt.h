// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// No salt: the SPIR-V handed to the driver is the module the port's caches hold.
#include <span>
#include <vector>
#include "common/types.h"

namespace iss::shader_salt {
inline u32 Value() {
    return 0;
}
/// The words to create the module from: `code` itself, or a salted copy kept in `storage`.
inline std::span<const u32> Apply(std::span<const u32> code, std::vector<u32>& storage) {
    const u32 salt = Value();
    if (!salt || code.size() < 5) return code;
    storage.assign(code.begin(), code.end());
    storage[2] ^= salt;
    storage[3] += 1 + (salt & 0xF);
    return storage;
}
} // namespace iss::shader_salt
