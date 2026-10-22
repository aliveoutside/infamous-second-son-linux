// SPDX-License-Identifier: GPL-2.0-or-later
// Player settings (~/.config/iss-pc/settings.toml) and the tuned defaults of the renderer.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// Call first thing in main(). Re-executes the program once if the dynamic loader needs a setting
// (GLIBC_TUNABLES), then fills the process environment, which the runtime and libiss-gpu.so read:
// a variable already set in the environment wins, then settings.toml, then the built-in defaults.
// Returns 0, or -1 when the settings file has an error (printed).
int settings_apply(int argc, char **argv);

// The player settings, in file order (also what the launcher, launcher/launcher.cpp, edits).
typedef struct {
    const char *key, *variable, *fallback, *comment;
    const char *const *choices; // NULL: any value; else "value=variable value" pairs
} SettingsEntry;
extern const SettingsEntry settings_entries[];
extern const int settings_entry_count;
enum { SETTINGS_VALUE_SIZE = 1024 };

// The settings file: $ISS_SETTINGS, else $XDG_CONFIG_HOME/iss-pc/settings.toml.
void settings_path(char *out, size_t size);
// The data folder (caches, saves): $ISS_USER_DIR, else user/ next to the program, else
// $XDG_DATA_HOME/iss-pc (created).
void settings_user_dir(char *out, size_t size);
// values[i]: settings_entries[i] from the file, or its default. -1: no file (all defaults).
int settings_load(char (*values)[SETTINGS_VALUE_SIZE]);
// 1 when value is accepted for settings_entries[index] (empty: the code's own default).
int settings_valid(int index, const char *value);
// Writes every setting with its comment; the [developer] section of the old file is kept.
int settings_save(const char (*values)[SETTINGS_VALUE_SIZE]);

#ifdef __cplusplus
}
#endif
