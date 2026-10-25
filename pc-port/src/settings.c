// SPDX-License-Identifier: GPL-2.0-or-later
// Player settings and the renderer's tuned defaults (settings.h).
//
// The runtime and libiss-gpu.so read their options from the process environment (getenv). The
// player never sets any of them: this file fills them in before anything reads them, from
//   1. the environment itself (developer overrides: ISS_FOO=1 ./iss-probe ...),
//   2. ~/.config/iss-pc/settings.toml (written with the defaults on the first start),
//   3. the built-in defaults below.
#include "settings.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// The tested play configuration (Linux, burning house and city, 2026-10-21): the approved profile
// of the old Windows launcher plus the settings measured since. Notes on the measured ones:
// - ISS_SUBMIT_INTERVAL_US: submit the GPU batch every 0.5 ms while recording, so a guest read
//   waits ~2 ms instead of ~16 ms.
// - ISS_ASYNC_COMPUTE/ISS_COMPUTE_FAIRNESS off: with the submit interval, async compute measured
//   worse on RADV (~50.5 vs ~52.6 FPS, ~300 vs ~220 frames over 33 ms per minute).
// - ISS_CP_CPU=auto: the command processor thread (the frame-rate limit) gets the fastest core.
// - BB_VBLANK_HZ=240: the guest flips on the emulated vblank; at 60 Hz a 20 ms frame waited for
//   33.3 ms. fps_limit below keeps the top.
// - ISS_GPU_BLACKBOX off: the GPU crash recorder costs ~3% of the command processor.
// - ISS_GC_TRIGGER_MB: the renderer reads a 752 MB texture budget on an 8 GB RADV card and
//   collected textures every frame; a fixed trigger, the fallback when the driver reports no usage.
// - ISS_GC_BUDGET_MB=auto: the texture budget is what the card has left after the arena, staging
//   and swapchain, measured live (ported from iss_pc), instead of a fixed trigger that the
//   game's 5 GB of use always exceeded: ~28k textures were deleted and uploaded again per session.
// - ISS_FAST_DRAW/ISS_FAST_REBIND: draws that repeat the previous draw's state record only the
//   geometry; city view with ~13k draws per frame 22.7 -> 34.2 FPS, no ISS_FAST_DRAW_VERIFY
//   mismatches (2026-10-22).
static const char *const tuned_defaults[][2] = {
    {"ISS_GPU", "1"}, {"ISS_WINDOW_SIZE", "1920x1080"},
    {"ISS_SUBMIT_INTERVAL_US", "500"}, {"ISS_CP_CPU", "auto"}, {"BB_VBLANK_HZ", "240"},
    {"ISS_GPU_BLACKBOX", "0"}, {"ISS_GC_TRIGGER_MB", "3500"}, {"ISS_GC_BUDGET_MB", "auto"},
    {"ISS_RING_BARRIERS", "1"}, {"ISS_REAL_OCCLUSION", "1"}, {"ISS_CP_SPLIT", "0"},
    {"ISS_PREWARM_LARGE_UPLOAD", "0"}, {"ISS_MINIMAL_LOGS", "1"}, {"ISS_GPU_PROFILE", "0"},
    {"ISS_RENDER_REUSE", "1"}, {"ISS_PARALLEL_RENDER", "1"}, {"ISS_DESCRIPTOR_FAST_PATH", "1"},
    {"ISS_FAULT_WINDOW_KB", "64"}, {"ISS_UPLOAD_WORKER", "1"},
    {"ISS_FAST_DRAW", "1"}, {"ISS_FAST_REBIND", "1"},
    {"ISS_TUNING", "async_written_uploads=1,scout=1,scout_fast=1,bind_prefetch=1"},
    {"ISS_BARRIER_POOL", "1"}, {"ISS_MAPPING_CACHE", "1"}, {"SHADPS4_FAST_STAGE_LOOKUP", "1"},
    {"ISS_TLS_PATCH", "1"}, {"ISS_THREAD_PRIORITY", "1"}, {"ISS_MAPPING_INDEX", "1"},
    {"ISS_COMPUTE_PHASES", "0"}, {"ISS_COMMAND_TRACE", "0"}, {"ISS_CAPTURE_FRAMES", "0"},
    {"ISS_DEEP_PROFILE", "0"}, {"ISS_FEEDBACK_TRACE", "0"}, {"ISS_FEEDBACK_IDENTITY", "0"},
    {"ISS_G4_FEEDBACK_PROBE", "0"}, {"ISS_READBACK_DIAG", "0"}, {"ISS_TRACE_READBACK", "0"},
    {"ISS_GPU_HAZARD_DIAG", "0"}, {"ISS_NATIVE_FEEDBACK_PROBE", "0"}, {"ISS_PROBE_HOST_IMPORT", "0"},
    {"ISS_ASYNC_COMPUTE", "0"}, {"ISS_COMPUTE_FAIRNESS", "0"}, {"ISS_FEEDBACK_EARLY_SUBMIT", "0"},
    {"ISS_FEEDBACK_PAGE_READBACK", "0"}, {"ISS_NATIVE_REDUCTION", "0"}, {"ISS_PIPELINE_EVENTS", "0"},
    {"ISS_NATIVE_READBACK_PAGES", "0"}, {"ISS_NATIVE_INSTANCED_BATCH", "0"},
    {"ISS_FEEDBACK_ARENA", "0"}, {"ISS_FEEDBACK_JOIN", "0"}, {"ISS_SECONDARY_BATCH_PROBE", "0"},
    {"ISS_NATIVE_MEMORY_REPORT", "0"}, {"ISS_NATIVE_INSTANCED_FEATURE", "0"}, {"ISS_TIMELINE", "0"},
    {"ISS_NATIVE_CPU_DRAWS", "0"}, {"ISS_CPU_BUNDLE_SPV_VERIFIED", "0"},
    {"ISS_CLAMP_CACHE_FIRST", "0"}, {"ISS_CPU_BUNDLE_INDEX_ONCE", "0"}, {"ISS_SMALL_READ_MEMO", "0"},
    {"ISS_DESCRIPTOR_DELTAS", "0"}, {"ISS_TEXTURE_LOOKUP_MEMO", "0"},
    {"ISS_TEXTURE_LOOKUP_MEMO_STATS", "0"}, {"ISS_PIPELINE_STATE_REUSE", "0"},
    {"ISS_PIPELINE_STATE_REUSE_VERIFY", "0"}, {"ISS_PIPELINE_STATE_REUSE_STATS", "0"},
    {"ISS_VISUAL_SEQUENCE", "0"},
};

// What a player sets (SettingsEntry, settings.h). Each maps to one variable; the values are checked
// here so a typo is reported at start instead of silently ignored by the code that reads the variable.
typedef SettingsEntry Setting;
static const char *const on_off[] = {"on=1", "off=0", NULL};
static const char *const vsync_choices[] = {"on=Fifo", "off=Immediate", "mailbox=Mailbox", NULL};
static const char *const memory_choices[] = {"video=0", "in_place=1", NULL};
static const char *const shader_choices[] = {"wait=0", "parallel=1", "skip=2", NULL};
static const char *const precompile_choices[] = {"on=auto", "off=", NULL};
static const char *const perf_choices[] = {"off=off", "low=low", "office=office", NULL};
static const char *const hint_choices[] = {"auto=auto", "always=always", "off=off", NULL};
static const char *const fsr_mode_choices[] = {"native=1.0", "quality=1.5", "balanced=1.7",
                                               "performance=2.0", "ultra_performance=3.0", NULL};
static const char *const hud_choices[] = {"16:9=1.7778", "21:9=2.3704", "full=0", NULL};
static const char *const language_choices[] = {
    "english=1", "french=2", "spanish=3", "german=4", "italian=5", "dutch=6", "portuguese=7",
    "russian=8", "finnish=12", "swedish=13", "danish=14", "norwegian=15", "polish=16", "brazilian=17",
    "turkish=19", "latino=20", NULL};
const Setting settings_entries[] = {
    {"game_dir", "ISS_GAME_DIR", "", "The game folder (the one with eboot.bin).", NULL},
    {"save_dir", "ISS_SAVE_ROOT", "", "Where saves go. Empty: <data folder>/savedata.", NULL},
    {"resolution", "ISS_RESOLUTION", "1920x1080",
     "Render size, WIDTHxHEIGHT. Wider than 16:9 shows more to the sides (Hor+).", NULL},
    {"fullscreen", "ISS_FULLSCREEN", "off", "on or off. Alt+Enter switches in game.", on_off},
    {"vsync", "BB_PRESENT_MODE", "mailbox", "on, off (lowest latency, tears) or mailbox.", vsync_choices},
    {"fps_limit", "BB_FPS_LIMIT", "60", "Frame rate limit, 0 = unlimited.", NULL},
    {"fsr_aa", "ISS_FSR", "off",
     "on: FSR 3 replaces the game's own anti-aliasing (SMAA T2x), at the render size. Also in the F1 menu.",
     on_off},
    {"fsr_mode", "ISS_FSR_MODE", "native",
     "The 3D picture's size, the output size divided by: native (1), quality (1.5), balanced (1.7), "
     "performance (2) or ultra_performance (3). With fsr_aa FSR 3 scales it up, without it the game "
     "does. Read at start. Also in the F1 menu.", fsr_mode_choices},
    {"fsr_sharpness", "ISS_FSR_SHARPNESS", "0",
     "FSR 3 sharpening in percent (0 = off, 100 = strongest), with fsr_aa. Also in the F1 menu.", NULL},
    {"frame_generation", "ISS_FG", "off",
     "on: FSR 3 frame generation, a frame shown between two of the game's (needs fsr_aa). Also in the "
     "F1 menu.", on_off},
    {"mangohud", "ISS_MANGOHUD", "off", "on: the launcher starts the game under MangoHud (if installed).", on_off},
    {"fov", "ISS_FOV", "100", "Field of view in percent of the game's (50 to 200).", NULL},
    {"hud_width", "ISS_HUD_ASPECT", "16:9", "HUD edges on screens wider than 16:9: 16:9, 21:9 or full.", hud_choices},
    {"language", "ISS_LANGUAGE", "english",
     "english french italian german spanish dutch portuguese russian polish turkish (voices), "
     "danish finnish norwegian swedish (subtitles only); North American edition: "
     "english french latino brazilian.", language_choices},
    {"gpu_memory", "ISS_UNIFIED_MEMORY", "video",
     "video: the GPU reads copies in video memory. in_place: reads the game's memory directly "
     "(much slower on NVIDIA).", memory_choices},
    {"shaders", "ISS_ASYNC_SHADERS", "wait",
     "New shaders: wait (the frame stops while one compiles), parallel (a burst compiles on more "
     "threads, still waited for) or skip (objects using it are not drawn until it is ready).",
     shader_choices},
    {"shader_precompile", "ISS_AOT_COMPUTE", "off",
     "on: every compute shader of the game is compiled ahead of time, in the background while "
     "playing (the first start indexes the game files). off: compiled when first used.",
     precompile_choices},
    {"native_passes", "ISS_NATIVE_PASSES", "on",
     "on: the engine's own threads record whole render passes into Vulkan, so the command processor "
     "thread (the limit in busy city views) translates far fewer draws. Switch it off if the GPU driver "
     "crashes (NVIDIA 595 did in iss_pc's tests).", on_off},
    {"performance_mode", "ISS_PERF_MODE", "off",
     "The game's own lower quality settings for slower PCs: off, low (shorter LOD distances, one "
     "sun-shadow cascade less, half-resolution particles) or office (LOD 0.5, quarter-resolution "
     "particles, no high-res texture blocks, half the pedestrians). 1.07 eboot only.", perf_choices},
    {"mouse_aim", "ISS_MOUSE_AIM", "on",
     "on: the mouse looks and aims as soon as the game window has focus (left button fires, right "
     "button aims). F4 switches it off and on in game.", on_off},
    {"mouse_sensitivity", "ISS_MOUSE_SENS", "100", "Mouse look speed in percent (10 to 1000).", NULL},
    {"hints", "ISS_HINTS", "auto",
     "On-screen help for the touchpad (hold Alt) and the spray can (graffiti): auto (in full the first "
     "three times, then a small label), always or off. Also in the F5 panel.", hint_choices},
    {"deadzone_left", "ISS_PAD_DEADZONE_LEFT", "10",
     "Left stick dead zone in percent of full deflection (0 to 50).", NULL},
    {"deadzone_right", "ISS_PAD_DEADZONE_RIGHT", "10",
     "Right stick dead zone in percent of full deflection (0 to 50).", NULL},
    {"pad_map", "ISS_PAD_MAP", "",
     "Controller layout for pads without a touchpad, e.g. \"view=touch_left,menu=touch_right\". "
     "Empty: the default.", NULL},
};
enum { setting_count = sizeof(settings_entries) / sizeof(settings_entries[0]) };
// Settings that no longer exist, skipped without an error (the next save drops them). fsr: the
// FSR 1 window upscaling, removed.
static const char *const retired_keys[] = {"fsr", NULL};
static int retired(const char *key) {
    for (const char *const *r = retired_keys; *r; ++r)
        if (!strcmp(*r, key)) return 1;
    return 0;
}
const int settings_entry_count = setting_count;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

// A TOML value as this file uses it: "string", 'string' or a bare word/number, # comments.
static int parse_value(char *raw, char *out, size_t size) {
    raw = trim(raw);
    if (*raw == '"' || *raw == '\'') {
        const char quote = *raw++;
        size_t n = 0;
        for (; *raw && *raw != quote; ++raw) {
            if (quote == '"' && *raw == '\\' && raw[1]) ++raw;
            if (n + 1 < size) out[n++] = *raw;
        }
        out[n] = 0;
        if (*raw != quote) return -1;
        char *rest = trim(raw + 1);
        return *rest && *rest != '#' ? -1 : 0;
    }
    char *hash = strchr(raw, '#');
    if (hash) *hash = 0;
    snprintf(out, size, "%s", trim(raw));
    return 0;
}

static const char *choice_value(const Setting *s, const char *value) {
    if (!s->choices) return value;
    const size_t length = strlen(value);
    for (const char *const *c = s->choices; *c; ++c)
        if (!strncasecmp(*c, value, length) && (*c)[length] == '=') return *c + length + 1;
    return NULL;
}

static int check_value(const Setting *s, const char *value) {
    if (s->choices && !choice_value(s, value)) return 0;
    if (!strcmp(s->key, "resolution")) {
        unsigned w = 0, h = 0;
        char extra;
        return sscanf(value, "%ux%u%c", &w, &h, &extra) == 2 && w >= 640 && h >= 360 && w <= 7680 && h <= 4320;
    }
    if (!strcmp(s->key, "fps_limit") || !strcmp(s->key, "fov") || !strcmp(s->key, "mouse_sensitivity") || !strncmp(s->key, "deadzone", 8) ||
        !strcmp(s->key, "fsr_sharpness")) {
        char *end;
        const long n = strtol(value, &end, 10);
        if (*end || !*value) return 0;
        if (!strncmp(s->key, "deadzone", 8)) return n >= 0 && n <= 50;
        if (!strcmp(s->key, "mouse_sensitivity")) return n >= 10 && n <= 1000;
        if (!strcmp(s->key, "fsr_sharpness")) return n >= 0 && n <= 100;
        return !strcmp(s->key, "fov") ? n >= 50 && n <= 200 : n >= 0 && n <= 1000;
    }
    return 1;
}

static void config_path(char *out, size_t size, const char *name) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(out, size, "%s/iss-pc%s%s", xdg, name ? "/" : "", name ? name : "");
    else snprintf(out, size, "%s/.config/iss-pc%s%s", getenv("HOME") ? getenv("HOME") : ".", name ? "/" : "", name ? name : "");
}

static const char developer_header[] =
    "# Developer overrides: any internal variable, e.g. ISS_GPU_PROFILE = \"1\".\n"
    "# The same variable set in the environment wins over this file.\n[developer]\n";

// The file with every setting and its comment; values NULL: the defaults. developer: the lines
// after [developer] to keep (NULL: none).
static int write_file(const char *path, const char (*values)[SETTINGS_VALUE_SIZE], const char *developer) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + 1; *p; ++p)
        if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    char temporary[4200];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *f = fopen(temporary, "w");
    if (!f) return -1;
    fputs("# inFAMOUS Second Son PC port settings. Delete this file to get the defaults back.\n\n", f);
    for (int i = 0; i < setting_count; ++i) {
        const Setting *s = &settings_entries[i];
        fprintf(f, "# %s\n%s = \"", s->comment, s->key);
        for (const char *c = values ? values[i] : s->fallback; *c; ++c) {
            if (*c == '"' || *c == '\\') fputc('\\', f);
            fputc(*c, f);
        }
        fputs("\"\n\n", f);
    }
    fputs(developer_header, f);
    if (developer) fputs(developer, f);
    if (fclose(f)) { unlink(temporary); return -1; }
    return rename(temporary, path);
}

void settings_path(char *out, size_t size) {
    const char *custom = getenv("ISS_SETTINGS");
    if (custom && *custom) snprintf(out, size, "%s", custom);
    else config_path(out, size, "settings.toml");
}

int settings_valid(int index, const char *value) {
    if (index < 0 || index >= setting_count) return 0;
    return !*value || check_value(&settings_entries[index], value);
}

int settings_load(char (*values)[SETTINGS_VALUE_SIZE]) {
    for (int i = 0; i < setting_count; ++i) snprintf(values[i], SETTINGS_VALUE_SIZE, "%s", settings_entries[i].fallback);
    char path[4096];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[4096];
    int developer = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        char value[SETTINGS_VALUE_SIZE];
        if (*s == '[') developer = !strncmp(s, "[developer]", 11);
        if (developer || !*s || *s == '#' || *s == '[' || !eq) continue;
        *eq = 0;
        const char *key = trim(s);
        if (parse_value(eq + 1, value, sizeof(value))) continue;
        for (int i = 0; i < setting_count; ++i)
            if (!strcmp(settings_entries[i].key, key)) snprintf(values[i], SETTINGS_VALUE_SIZE, "%s", value);
    }
    fclose(f);
    return 0;
}

int settings_save(const char (*values)[SETTINGS_VALUE_SIZE]) {
    char path[4096];
    settings_path(path, sizeof(path));
    // Keep what the player wrote under [developer].
    char *developer = NULL;
    size_t length = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        FILE *kept = open_memstream(&developer, &length);
        char line[4096];
        int inside = 0;
        while (fgets(line, sizeof(line), f)) {
            char copy[sizeof(line)];
            char *s = trim(strcpy(copy, line));
            if (*s == '[') { inside = !strncmp(s, "[developer]", 11); continue; }
            // The header comment is written again by write_file.
            if (inside && strncmp(s, "# Developer overrides:", 22) && strncmp(s, "# The same variable set", 23)) fputs(line, kept);
        }
        fclose(kept);
        fclose(f);
    }
    const int result = write_file(path, values, developer);
    free(developer);
    return result;
}

// ISS_USER_DIR: the folder next to the program when it already has one (old installs keep their
// saves and caches), else ~/.local/share/iss-pc.
void settings_user_dir(char *out, size_t size) {
    const char *set = getenv("ISS_USER_DIR");
    if (set && *set) { snprintf(out, size, "%s", set); return; }
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 8);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            strcpy(slash, "/user");
            struct stat st;
            if (!stat(exe, &st) && S_ISDIR(st.st_mode)) { snprintf(out, size, "%s", exe); return; }
        }
    }
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) snprintf(out, size, "%s/iss-pc", xdg);
    else {
        char share[4096];
        snprintf(share, sizeof(share), "%s/.local/share", getenv("HOME") ? getenv("HOME") : ".");
        mkdir(share, 0755);
        snprintf(out, size, "%s/iss-pc", share);
    }
    mkdir(out, 0755);
}

static void default_user_dir(void) {
    if (getenv("ISS_USER_DIR")) return;
    char dir[4096];
    settings_user_dir(dir, sizeof(dir));
    setenv("ISS_USER_DIR", dir, 0);
}

int settings_apply(int argc, char **argv) {
    (void)argc;
    // libiss-gpu.so uses the initial-exec TLS model: without spare static TLS (~46 KB used) the
    // load fails. The loader reads the tunable only at start, so set it and start again.
    const char *tunables = getenv("GLIBC_TUNABLES");
    if (!tunables || !strstr(tunables, "glibc.rtld.optional_static_tls=")) {
        char value[2048];
        snprintf(value, sizeof(value), "%s%sglibc.rtld.optional_static_tls=65536", tunables ? tunables : "",
                 tunables && *tunables ? ":" : "");
        setenv("GLIBC_TUNABLES", value, 1);
        execv("/proc/self/exe", argv);
        fprintf(stderr, "settings: restart for GLIBC_TUNABLES failed (%s)\n", strerror(errno));
    }

    char path[4096];
    const char *custom = getenv("ISS_SETTINGS");
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f && !(custom && *custom)) {
        write_file(path, NULL, NULL);
        fprintf(stderr, "settings: wrote the defaults to %s\n", path);
        f = fopen(path, "r");
    }
    int errors = 0;
    char player[setting_count][SETTINGS_VALUE_SIZE];
    int have[setting_count] = {0};
    if (f) {
        char line[4096];
        int number = 0, developer = 0;
        while (fgets(line, sizeof(line), f)) {
            ++number;
            char *s = trim(line);
            if (!*s || *s == '#') continue;
            if (*s == '[') { developer = !strncmp(s, "[developer]", 11); continue; }
            char *eq = strchr(s, '=');
            char value[SETTINGS_VALUE_SIZE];
            if (!eq) { fprintf(stderr, "settings: %s:%d: expected key = value\n", path, number); ++errors; continue; }
            *eq = 0;
            char *key = trim(s);
            if (parse_value(eq + 1, value, sizeof(value))) {
                fprintf(stderr, "settings: %s:%d: bad value for %s\n", path, number, key); ++errors; continue;
            }
            if (developer) { setenv(key, value, 0); continue; }
            int found = 0;
            for (int i = 0; i < setting_count; ++i)
                if (!strcmp(settings_entries[i].key, key)) { snprintf(player[i], sizeof(player[i]), "%s", value); have[i] = found = 1; }
            if (!found && !retired(key)) { fprintf(stderr, "settings: %s:%d: unknown setting %s\n", path, number, key); ++errors; }
        }
        fclose(f);
    }
    int fps_limit = -1;
    for (int i = 0; i < setting_count; ++i) {
        const Setting *s = &settings_entries[i];
        const char *value = have[i] ? player[i] : s->fallback;
        if (!*value) continue; // empty: the code's own default
        const char *mapped = choice_value(s, value);
        if (!mapped || !check_value(s, value)) {
            fprintf(stderr, "settings: %s = \"%s\" is not valid (%s) - using \"%s\"\n", s->key, value, s->comment, s->fallback);
            ++errors;
            value = s->fallback;
            if (!*value) continue;
            mapped = choice_value(s, value);
        }
        setenv(s->variable, mapped, 0);
        if (!strcmp(s->key, "fps_limit")) fps_limit = atoi(getenv(s->variable));
    }
    // native_passes: the profile the original launcher ran it with (all of it needs the first).
    if (getenv("ISS_NATIVE_PASSES") && !strcmp(getenv("ISS_NATIVE_PASSES"), "1")) {
        static const char *const native_profile[][2] = {
            {"ISS_NATIVE_PASSES_ASYNC_SCAN", "1"}, {"ISS_NATIVE_PASSES_DYNAMIC", "1"},
            {"ISS_NATIVE_PASSES_SUBMIT", "1"}, {"ISS_NATIVE_MATERIALS", "1"}, {"ISS_NATIVE_ASC_ASYNC", "1"},
            {"ISS_NATIVE_GBUFFER_SETS", "1"}, {"ISS_NATIVE_INSTANCED_FEATURE", "1"}, {"ISS_SPEC_TRANSLATE", "1"},
            {"ISS_STAGE_MEMO", "1"}, {"ISS_STAGE_MEMO_LOOSE", "1"}, {"ISS_STAGE_MEMO_TEXEL", "1"},
        };
        for (size_t i = 0; i < sizeof(native_profile) / sizeof(native_profile[0]); ++i)
            setenv(native_profile[i][0], native_profile[i][1], 0);
    }
    // shaders: parallel compiles new graphics pipelines on the compiler threads (waited for on
    // the bind); skip also draws nothing with them until they are ready (full screen passes wait).
    if (getenv("ISS_ASYNC_SHADERS")) {
        const int mode = atoi(getenv("ISS_ASYNC_SHADERS"));
        if (mode >= 1) setenv("ISS_LAZY_PIPELINES", "1", 0);
        if (mode >= 2) setenv("ISS_ASYNC_SKIP_DRAWS", "1", 0);
    }
    // Flips are taken on vblank ticks (240 Hz): a limit that does not divide it snaps to the next
    // tick (75 ran at 80 FPS, 144 at 120), so the vblank runs at a multiple of the limit.
    if (fps_limit > 0) {
        char hz[32];
        snprintf(hz, sizeof(hz), "%d", fps_limit * ((240 + fps_limit - 1) / fps_limit));
        setenv("BB_VBLANK_HZ", hz, 0);
    }
    for (size_t i = 0; i < sizeof(tuned_defaults) / sizeof(tuned_defaults[0]); ++i)
        setenv(tuned_defaults[i][0], tuned_defaults[i][1], 0);
    default_user_dir();
    return errors ? -1 : 0;
}
