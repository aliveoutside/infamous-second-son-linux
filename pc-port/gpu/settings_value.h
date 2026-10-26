// SPDX-License-Identifier: GPL-2.0-or-later
// The player's settings (settings.toml) as libiss-gpu.so reads them: settings_get of src/settings.h,
// exported by the executable that loads this library (iss-probe). Usable while the library loads.
#pragma once

#include <dlfcn.h>

namespace iss {

/// The value of a settings.toml key for the code (a choice's code value, "1"/"0" for on/off), or
/// nullptr: empty, unknown, or no executable exporting the settings (the tests).
inline const char* Setting(const char* key) {
    using Get = const char* (*)(const char*);
    static const Get get = reinterpret_cast<Get>(dlsym(RTLD_DEFAULT, "settings_get"));
    return get ? get(key) : nullptr;
}

/// A key changed while playing (the in-game menu): what Setting returns from now on.
inline void SetSetting(const char* key, const char* value) {
    using Set = int (*)(const char*, const char*);
    static const Set set = reinterpret_cast<Set>(dlsym(RTLD_DEFAULT, "settings_set"));
    if (set) set(key, value);
}

} // namespace iss
