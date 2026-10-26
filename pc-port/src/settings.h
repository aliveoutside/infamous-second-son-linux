// SPDX-License-Identifier: GPL-2.0-or-later
// Player settings (~/.config/iss-pc/settings.toml) and the tuned defaults of the renderer.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// Call first thing in main(). Re-executes the program once if the dynamic loader needs a setting
// (GLIBC_TUNABLES), then reads settings.toml (the built-in default for a key it does not set) into
// the values settings_get returns. argv[1], the prepared image, finds game_dir when it is empty
// (game_dir.txt next to it, written by setup). Returns 0, or -1 when the settings file has an error
// (printed).
int settings_apply(int argc, char **argv);

// The value of a key for the code that uses it: the choice's code value ("on" is "1", "mailbox"
// "Mailbox", see the table), else the text. NULL when empty or unknown: the code's own default.
// libiss-gpu.so finds it in the executable (gpu/settings_value.h). Valid until the next settings_set
// of the same key.
const char *settings_get(const char *key);
// A key changed while playing (the F1 menu, the F5 panel): what settings_get returns from now on.
// The file is written separately (settings_save). Returns 0, or -1 for an unknown key or a bad value.
int settings_set(const char *key, const char *value);

// The player settings, in file order (also what the launcher, launcher/launcher.cpp, edits).
typedef struct {
    const char *key, *fallback, *comment;
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
