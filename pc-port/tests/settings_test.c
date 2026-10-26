// SPDX-License-Identifier: GPL-2.0-or-later
// settings.toml as the launcher edits it: defaults without a file, a save keeps the player's
// [developer] lines and quotes, the values are checked like settings_apply checks them.
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static int index_of(const char *key) {
    for (int i = 0; i < settings_entry_count; ++i)
        if (!strcmp(settings_entries[i].key, key)) return i;
    return -1;
}

int main(void) {
    char dir[] = "/tmp/iss-settings-XXXXXX", path[256];
    if (!mkdtemp(dir)) return 1;
    snprintf(path, sizeof(path), "%s/sub/settings.toml", dir);
    setenv("ISS_SETTINGS", path, 1);

    static char values[64][SETTINGS_VALUE_SIZE];
    const int fov = index_of("fov"), resolution = index_of("resolution"), pad = index_of("pad_map"), hud = index_of("hud_width");
    CHECK(fov >= 0 && resolution >= 0 && pad >= 0 && hud >= 0);
    CHECK(settings_load(values) == -1);
    CHECK(!strcmp(values[fov], "100") && !strcmp(values[resolution], "1920x1080"));

    CHECK(settings_valid(fov, "110") && !settings_valid(fov, "20") && !settings_valid(fov, "abc"));
    CHECK(settings_valid(resolution, "2560x1080") && !settings_valid(resolution, "100x100"));
    CHECK(settings_valid(hud, "21:9") && !settings_valid(hud, "4:3"));
    const int mango = index_of("mangohud");
    CHECK(mango >= 0 && !strcmp(values[mango], "off"));
    CHECK(settings_valid(mango, "on") && !settings_valid(mango, "yes"));
    const int aim = index_of("mouse_aim");
    CHECK(aim >= 0 && !strcmp(values[aim], "on") && settings_valid(aim, "off") && !settings_valid(aim, "maybe"));
    const int hints = index_of("hints");
    CHECK(hints >= 0 && !strcmp(values[hints], "auto") && settings_valid(hints, "off") && settings_valid(hints, "always") && !settings_valid(hints, "never"));
    const int perf = index_of("performance_mode"), native = index_of("native_passes");
    CHECK(perf >= 0 && !strcmp(values[perf], "off") && settings_valid(perf, "low") && settings_valid(perf, "office") && !settings_valid(perf, "high"));
    CHECK(native >= 0 && !strcmp(values[native], "on") && settings_valid(native, "off") && !settings_valid(native, "maybe"));
    // FSR 3 (fsr_aa, frame_generation) replaced the FSR 1 upscaling (fsr, retired).
    const int fsr_aa = index_of("fsr_aa"), fg = index_of("frame_generation");
    CHECK(fsr_aa >= 0 && !strcmp(values[fsr_aa], "off") && settings_valid(fsr_aa, "on") && !settings_valid(fsr_aa, "yes"));
    CHECK(fg >= 0 && !strcmp(values[fg], "off") && settings_valid(fg, "on") && !settings_valid(fg, "2x"));
    CHECK(index_of("fsr") < 0);
    const int sharp = index_of("fsr_sharpness");
    CHECK(index_of("fsr_mode") < 0);
    CHECK(sharp >= 0 && !strcmp(values[sharp], "0") && settings_valid(sharp, "100") && !settings_valid(sharp, "101"));
    const int sens = index_of("mouse_sensitivity");
    CHECK(sens >= 0 && !strcmp(values[sens], "100") && settings_valid(sens, "300") && !settings_valid(sens, "5") && !settings_valid(sens, "x"));
    const int dead = index_of("deadzone_left"), dead_right = index_of("deadzone_right");
    CHECK(dead >= 0 && !strcmp(values[dead], "10") && dead_right >= 0 && !strcmp(values[dead_right], "10"));
    CHECK(settings_valid(dead, "0") && settings_valid(dead, "50") && !settings_valid(dead, "51") && !settings_valid(dead, "-1"));
    CHECK(settings_valid(dead_right, "25") && !settings_valid(dead_right, "abc"));

    // The Steam Deck's defaults for a new file: 16:10 fullscreen, Options on Menu, the back buttons.
    static char deck[64][SETTINGS_VALUE_SIZE];
    settings_load(deck);
    settings_apply_deck_defaults(deck);
    CHECK(!strcmp(deck[resolution], "1280x800") && !strcmp(deck[index_of("fullscreen")], "on"));
    CHECK(!strcmp(deck[pad], "menu=options,l4=touch_right,l5=touch,r4=l3,r5=r3"));
    CHECK(settings_valid(resolution, deck[resolution]) && !strcmp(values[resolution], "1920x1080"));

    // What the code reads (settings_get): a choice's code value, empty is NULL, a bad value is refused.
    CHECK(settings_get("fsr_aa") == NULL);
    CHECK(settings_set("fsr_aa", "on") == 0 && !strcmp(settings_get("fsr_aa"), "1"));
    CHECK(settings_set("fsr_aa", "yes") == -1 && !strcmp(settings_get("fsr_aa"), "1"));
    CHECK(settings_set("vsync", "mailbox") == 0 && !strcmp(settings_get("vsync"), "Mailbox"));
    CHECK(settings_set("fov", "110") == 0 && !strcmp(settings_get("fov"), "110"));
    CHECK(settings_set("fov", "20") == -1 && settings_set("no_such_key", "1") == -1 && !settings_get("no_such_key"));
    CHECK(settings_set("save_dir", "") == 0 && settings_get("save_dir") == NULL);

    strcpy(values[fov], "115");
    strcpy(values[pad], "a=\"q\\uote");
    CHECK(settings_save((const char (*)[SETTINGS_VALUE_SIZE])values) == 0);
    FILE *f = fopen(path, "a");
    CHECK(f != NULL);
    if (f) { fputs("[developer]\nISS_GPU_PROFILE = \"1\"\n", f); fclose(f); }

    strcpy(values[resolution], "2560x1080");
    CHECK(settings_save((const char (*)[SETTINGS_VALUE_SIZE])values) == 0);
    static char again[64][SETTINGS_VALUE_SIZE];
    CHECK(settings_load(again) == 0);
    CHECK(!strcmp(again[fov], "115") && !strcmp(again[resolution], "2560x1080"));
    CHECK(!strcmp(again[pad], "a=\"q\\uote"));

    // The developer line is kept once, the header once.
    f = fopen(path, "r");
    char line[4096];
    int developer = 0;
    while (f && fgets(line, sizeof(line), f)) developer += !strncmp(line, "[developer]", 11) || !strncmp(line, "ISS_", 4);
    if (f) fclose(f);
    CHECK(developer == 0);

    unlink(path);
    snprintf(path, sizeof(path), "%s/sub", dir);
    rmdir(path);
    rmdir(dir);
    if (!failures) puts("settings_test passed");
    return failures != 0;
}
