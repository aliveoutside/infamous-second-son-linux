// SPDX-License-Identifier: GPL-2.0-or-later
// inFAMOUS Second Son PC port launcher (Linux): edits settings.toml (src/settings.c, the same table
// the game reads) and starts the game through scripts/linux/play.sh.
//   <root>/build/iss-launcher        this program (root: the pc-port folder)
//   <root>/launcher/assets/fonts/    Oswald (OFL) for the titles; the body text is the system sans
// The window art is the game's own sce_sys/pic1.png, darkened on the left behind the menu. PLAY
// saves the settings, hides the window while the game runs and shows it again when the game exits;
// the game's output goes to <data folder>/logs/game.log (the previous run: game.prev.log).
// SDL3 and Dear ImGui (SDL renderer backend), both already used by the port.
#include "settings.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <SDL3/SDL.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include <fcntl.h>
#include <strings.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern char **environ;
namespace fs = std::filesystem;

namespace {

fs::path g_root;                          // the pc-port folder
char g_values[64][SETTINGS_VALUE_SIZE];   // what the form shows
char g_saved[64][SETTINGS_VALUE_SIZE];    // what the file has
std::string g_status;
bool g_status_error = false;

int Index(const char *key) {
    for (int i = 0; i < settings_entry_count; ++i)
        if (!std::strcmp(settings_entries[i].key, key)) return i;
    std::fprintf(stderr, "launcher: no setting %s in src/settings.c\n", key);
    std::abort();
}
char *Value(const char *key) { return g_values[Index(key)]; }
void Set(const char *key, const std::string &value) {
    std::snprintf(Value(key), SETTINGS_VALUE_SIZE, "%s", value.c_str());
}
bool Dirty() {
    for (int i = 0; i < settings_entry_count; ++i)
        if (std::strcmp(g_values[i], g_saved[i])) return true;
    return false;
}
void Status(std::string text, bool error = false) { g_status = std::move(text); g_status_error = error; }

// The game folder: game_dir, else the one scripts/linux/setup.sh recorded (as play.sh does).
std::string GameDir() {
    if (*Value("game_dir")) return Value("game_dir");
    std::ifstream recorded(g_root / "out" / "game_dir.txt");
    std::string line;
    std::getline(recorded, line);
    return line;
}
fs::path UserDir() {
    char dir[4096];
    settings_user_dir(dir, sizeof(dir));
    return dir;
}
fs::path SaveDir() { return *Value("save_dir") ? fs::path(Value("save_dir")) : UserDir() / "savedata"; }
fs::path LogFile() { return UserDir() / "logs" / "game.log"; }
fs::path SettingsFile() {
    char path[4096];
    settings_path(path, sizeof(path));
    return path;
}

// Helpers started with posix_spawn and reaped in the main loop (xdg-open).
std::vector<pid_t> g_helpers;
pid_t Spawn(const std::vector<std::string> &args, const fs::path *output = nullptr) {
    std::vector<char *> argv;
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (output) {
        posix_spawn_file_actions_addopen(&actions, 1, output->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
    }
    pid_t pid = -1;
    if (posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ)) pid = -1;
    posix_spawn_file_actions_destroy(&actions);
    return pid;
}
void Open(const fs::path &path) {
    std::error_code ignored;
    if (!fs::exists(path, ignored)) { Status("Not found: " + path.string(), true); return; }
    const pid_t pid = Spawn({"xdg-open", path.string()});
    if (pid > 0) g_helpers.push_back(pid);
    else Status("xdg-open is not available", true);
}

bool Save() {
    for (int i = 0; i < settings_entry_count; ++i)
        if (!settings_valid(i, g_values[i])) {
            Status(std::string("Not saved: ") + settings_entries[i].key + " = \"" + g_values[i] + "\" is not valid", true);
            return false;
        }
    if (settings_save(g_values)) { Status("Could not write " + SettingsFile().string(), true); return false; }
    std::memcpy(g_saved, g_values, sizeof(g_saved));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------------------------
pid_t g_game = -1;

// MangoHud is optional: the option only shows (and applies) when the binary is on PATH.
bool HaveMangoHud() {
    static const bool found = [] {
        const char *path = getenv("PATH");
        std::stringstream dirs(path ? path : "");
        for (std::string dir; std::getline(dirs, dir, ':');)
            if (!dir.empty() && !access((dir + "/mangohud").c_str(), X_OK)) return true;
        return false;
    }();
    return found;
}

void Play(SDL_Window *window) {
    if (!Save()) return;
    const fs::path script = g_root / "scripts" / "linux" / "play.sh";
    if (access(script.c_str(), X_OK)) { Status("Missing " + script.string(), true); return; }
    if (GameDir().empty() || !fs::exists(fs::path(GameDir()) / "eboot.bin")) {
        Status("Choose the game folder (the one with eboot.bin) under Folders.", true);
        return;
    }
    std::error_code ignored;
    fs::create_directories(LogFile().parent_path(), ignored);
    fs::rename(LogFile(), LogFile().parent_path() / "game.prev.log", ignored);
    const fs::path log = LogFile();
    setenv("ISS_GAME_DIR", GameDir().c_str(), 1);  // play.sh prepares this folder when needed
    std::vector<std::string> command = {script.string()};
    if (!std::strcmp(Value("mangohud"), "on") && HaveMangoHud()) command.insert(command.begin(), "mangohud");
    g_game = Spawn(command, &log);
    if (g_game < 0) { Status("Could not start " + script.string(), true); return; }
    SDL_HideWindow(window);
}

// Called every frame: the game's end brings the launcher back.
void Reap(SDL_Window *window) {
    for (auto it = g_helpers.begin(); it != g_helpers.end();)
        it = waitpid(*it, nullptr, WNOHANG) ? g_helpers.erase(it) : it + 1;
    if (g_game < 0) return;
    int status = 0;
    if (waitpid(g_game, &status, WNOHANG) != g_game) return;
    g_game = -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) Status("The game closed.");
    else if (WIFEXITED(status)) Status("The game ended with error " + std::to_string(WEXITSTATUS(status)) + ". See Open log.", true);
    else Status(std::string("The game stopped: ") + strsignal(WTERMSIG(status)) + ". See Open log.", true);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
}

// ---------------------------------------------------------------------------------------------
// Fonts and art
// ---------------------------------------------------------------------------------------------
// Unscaled sizes; style.FontScaleDpi scales them.
constexpr float kBody = 17, kHeading = 20;
ImFont *g_title = nullptr, *g_heading = nullptr, *g_body = nullptr;
SDL_Texture *g_background = nullptr;
int g_background_w = 0, g_background_h = 0;
std::string g_background_dir;

std::string SystemSans() {
    std::string path;
    if (FILE *p = popen("fc-match -f '%{file}' sans:style=Regular 2>/dev/null", "r")) {
        char buffer[4096];
        if (std::fgets(buffer, sizeof(buffer), p)) path = buffer;
        pclose(p);
    }
    return path;
}

void LoadFonts() {
    ImGuiIO &io = ImGui::GetIO();
    const std::string sans = SystemSans();
    if (!sans.empty() && fs::exists(sans)) g_body = io.Fonts->AddFontFromFileTTF(sans.c_str(), kBody);
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    const fs::path oswald = g_root / "launcher" / "assets" / "fonts" / "Oswald.ttf";
    if (fs::exists(oswald)) g_heading = io.Fonts->AddFontFromFileTTF(oswald.c_str(), kHeading);
    g_title = g_heading ? g_heading : g_body;
    if (!g_heading) g_heading = g_body;
    io.FontDefault = g_body;
}

// sce_sys/pic1.png of the game folder; loaded again when the folder changes.
void LoadBackground(SDL_Renderer *renderer) {
    const std::string dir = GameDir();
    if (dir == g_background_dir) return;
    g_background_dir = dir;
    if (g_background) { SDL_DestroyTexture(g_background); g_background = nullptr; }
    const fs::path pic = fs::path(dir) / "sce_sys" / "pic1.png";
    int w, h, channels;
    unsigned char *pixels = dir.empty() ? nullptr : stbi_load(pic.c_str(), &w, &h, &channels, 4);
    if (!pixels) return;
    g_background = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (g_background) {
        SDL_UpdateTexture(g_background, nullptr, pixels, w * 4);
        SDL_SetTextureScaleMode(g_background, SDL_SCALEMODE_LINEAR);
        g_background_w = w; g_background_h = h;
    }
    stbi_image_free(pixels);
}

void DrawBackground() {
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    draw->AddRectFilled({0, 0}, size, IM_COL32(12, 12, 16, 255));
    if (g_background) {
        // Cover the window, anchored right: the crow shows through on the right.
        const float scale = std::max(size.x / g_background_w, size.y / g_background_h);
        const float w = g_background_w * scale, h = g_background_h * scale;
        draw->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<std::uintptr_t>(g_background))), {size.x - w, (size.y - h) / 2},
                       {size.x, (size.y + h) / 2});
    }
    // The form covers the window: the art is dimmed under it, a little lighter on the right.
    draw->AddRectFilledMultiColor({0, 0}, size, IM_COL32(8, 8, 12, 235), IM_COL32(8, 8, 12, 190),
                                  IM_COL32(8, 8, 12, 190), IM_COL32(8, 8, 12, 235));
}

void Style(float scale) {
    ImGuiStyle &style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.WindowBorderSize = 0;
    style.FrameRounding = 3;
    style.TabRounding = 3;
    style.FramePadding = {10, 6};
    style.ItemSpacing = {10, 9};
    ImVec4 *c = style.Colors;
    const ImVec4 accent(0.80f, 0.16f, 0.20f, 1.f), accent_hover(0.92f, 0.24f, 0.27f, 1.f);
    c[ImGuiCol_WindowBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = {0.08f, 0.08f, 0.10f, 0.98f};
    c[ImGuiCol_FrameBg] = {1, 1, 1, 0.07f};
    c[ImGuiCol_FrameBgHovered] = {1, 1, 1, 0.12f};
    c[ImGuiCol_FrameBgActive] = {1, 1, 1, 0.16f};
    c[ImGuiCol_Button] = {1, 1, 1, 0.08f};
    c[ImGuiCol_ButtonHovered] = {1, 1, 1, 0.16f};
    c[ImGuiCol_ButtonActive] = {1, 1, 1, 0.22f};
    c[ImGuiCol_Header] = {accent.x, accent.y, accent.z, 0.45f};
    c[ImGuiCol_HeaderHovered] = {accent.x, accent.y, accent.z, 0.65f};
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Tab] = {1, 1, 1, 0.04f};
    c[ImGuiCol_TabHovered] = accent_hover;
    c[ImGuiCol_TabSelected] = accent;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hover;
    c[ImGuiCol_CheckMark] = accent_hover;
    c[ImGuiCol_NavCursor] = accent_hover;
    c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.03f};
    style.ScaleAllSizes(scale);
    style.FontSizeBase = kBody;
    style.FontScaleDpi = scale;
}

// ---------------------------------------------------------------------------------------------
// The form
// ---------------------------------------------------------------------------------------------
struct Choice { const char *label, *value; };

// A setting with fixed choices. A file value that is none of them is shown (and kept) as is.
void Combo(const char *label, const char *key, const std::vector<Choice> &choices) {
    char *value = Value(key);
    const Choice *current = nullptr;
    for (const auto &c : choices)
        if (!strcasecmp(c.value, value)) current = &c;
    const std::string preview = current ? current->label : std::string(value) + " (custom)";
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(key);
    if (ImGui::BeginCombo("##v", preview.c_str())) {
        for (const auto &c : choices) {
            const bool selected = &c == current;
            if (ImGui::Selectable(c.label, selected)) Set(key, c.value);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

bool BeginForm(const char *id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.5f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
void Label(const char *label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
}

const std::vector<Choice> kOnOff = {{"Off", "off"}, {"On", "on"}};

void DisplayTab() {
    if (!BeginForm("display")) return;
    Combo("Window", "fullscreen", {{"Windowed", "off"}, {"Fullscreen", "on"}});
    Combo("VSync", "vsync", {{"Mailbox", "mailbox"}, {"On", "on"}, {"Off", "off"}});
    Combo("Frame rate limit", "fps_limit",
          {{"Unlimited", "0"}, {"30", "30"}, {"40", "40"}, {"50", "50"}, {"60", "60"}, {"75", "75"}, {"90", "90"},
           {"100", "100"}, {"120", "120"}, {"144", "144"}, {"165", "165"}, {"240", "240"}});
    Combo("Anti-aliasing (FSR 3)", "fsr_aa", kOnOff);
    Label("Sharpening (FSR 3)");
    int sharpness = std::atoi(Value("fsr_sharpness"));
    if (sharpness < 0 || sharpness > 100) sharpness = 0;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##fsr_sharpness", &sharpness, 0, 100, "%d%%"))
        Set("fsr_sharpness", std::to_string(sharpness / 5 * 5));
    Combo("Frame generation (FSR 3)", "frame_generation", kOnOff);
    if (HaveMangoHud()) Combo("MangoHud", "mangohud", kOnOff);
    ImGui::EndTable();
}

void GraphicsTab() {
    if (!BeginForm("graphics")) return;
    // Resolution: the presets, the desktop's size, or any WxH.
    static std::vector<std::pair<std::string, std::string>> presets;
    if (presets.empty()) {
        if (const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
            const int w = desktop->w & ~7, h = desktop->h & ~7;
            const std::string size = std::to_string(w) + "x" + std::to_string(h);
            presets.push_back({"Desktop (" + size + ")", size});
        }
        for (const char *p : {"1280x800 (16:10, Steam Deck)", "1280x720 (16:9)", "1600x900 (16:9)", "1920x1080 (16:9)", "2560x1440 (16:9)",
                              "3840x2160 (16:9)", "1920x1200 (16:10)", "2560x1600 (16:10)", "2560x1080 (21:9)",
                              "3440x1440 (21:9)", "3840x1600 (24:10)", "3840x1080 (32:9)", "5120x1440 (32:9)"})
            presets.push_back({p, std::string(p, std::strchr(p, ' '))});
    }
    char *resolution = Value("resolution");
    static bool custom = false;
    std::string preview = std::string(resolution) + " (custom)";
    bool known = false;
    for (const auto &p : presets)
        if (p.second == resolution) { preview = p.first; known = true; break; }
    if (!known) custom = true;
    if (custom) preview = "Custom";
    Label("Resolution");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##resolution", preview.c_str())) {
        for (const auto &p : presets)
            if (ImGui::Selectable(p.first.c_str(), !custom && p.second == resolution)) { Set("resolution", p.second); custom = false; }
        if (ImGui::Selectable("Custom", custom)) custom = true;
        ImGui::EndCombo();
    }
    if (custom) {
        int w = 0, h = 0;
        std::sscanf(resolution, "%dx%d", &w, &h);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        bool changed = ImGui::InputInt("##w", &w, 0);
        ImGui::SameLine();
        ImGui::TextUnformatted("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::InputInt("##h", &h, 0);
        if (changed) Set("resolution", std::to_string(std::max(w, 0)) + "x" + std::to_string(std::max(h, 0)));
        if (!settings_valid(Index("resolution"), resolution)) {
            ImGui::SameLine();
            ImGui::TextColored({1, 0.4f, 0.4f, 1}, "640x360 to 7680x4320");
        }
    }

    Label("Field of view");
    int fov = std::atoi(Value("fov"));
    if (fov < 50 || fov > 200) fov = 100;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##fov", &fov, 50, 200, "%d%%")) Set("fov", std::to_string(fov / 5 * 5));

    Combo("HUD width", "hud_width", {{"16:9", "16:9"}, {"21:9", "21:9"}, {"Full width", "full"}});
    Combo("GPU memory", "gpu_memory", {{"Video memory", "video"}, {"In place", "in_place"}});
    Combo("New shaders", "shaders", {{"Wait", "wait"}, {"Parallel", "parallel"}, {"Skip draws", "skip"}});
    Combo("Precompile shaders", "shader_precompile", {{"Off", "off"}, {"On", "on"}});
    Combo("Native render passes", "native_passes", kOnOff);
    Combo("Performance mode", "performance_mode", {{"Off", "off"}, {"Low", "low"}, {"Office", "office"}});
    ImGui::EndTable();
}

void GameTab() {
    if (!BeginForm("game")) return;
    Combo("Language", "language",
          {{"English", "english"}, {"French", "french"}, {"Italian", "italian"}, {"German", "german"},
           {"Spanish", "spanish"}, {"Dutch", "dutch"}, {"Portuguese", "portuguese"}, {"Russian", "russian"},
           {"Polish", "polish"}, {"Turkish", "turkish"}, {"Danish (subtitles)", "danish"},
           {"Finnish (subtitles)", "finnish"}, {"Norwegian (subtitles)", "norwegian"},
           {"Swedish (subtitles)", "swedish"},
           {"Spanish, Latin America (NA edition)", "latino"}, {"Portuguese, Brazil (NA edition)", "brazilian"}});
    ImGui::EndTable();
}

// pad_map (src/pad.c): what each button of a controller without a touchpad does in game.
struct PadName { const char *key, *label; };
const PadName kSources[] = {
    {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"lb", "LB"}, {"rb", "RB"}, {"lt", "LT"}, {"rt", "RT"},
    {"ls", "Left stick press"}, {"rs", "Right stick press"}, {"view", "View"}, {"menu", "Menu"},
    {"guide", "Guide (Xbox button)"}, {"share", "Share"}, {"up", "D-pad up"}, {"down", "D-pad down"},
    {"left", "D-pad left"}, {"right", "D-pad right"},
    {"l4", "L4 (Steam Deck back, left upper)"}, {"l5", "L5 (Steam Deck back, left lower)"},
    {"r4", "R4 (Steam Deck back, right upper)"}, {"r5", "R5 (Steam Deck back, right lower)"}};
const PadName kTargets[] = {
    {"none", "Nothing"}, {"cross", "Cross"}, {"circle", "Circle"}, {"square", "Square"}, {"triangle", "Triangle"},
    {"l1", "L1"}, {"r1", "R1"}, {"l2", "L2"}, {"r2", "R2"}, {"l3", "L3"}, {"r3", "R3"}, {"options", "Options"},
    {"ps", "PS button"}, {"up", "D-pad up"}, {"down", "D-pad down"}, {"left", "D-pad left"},
    {"right", "D-pad right"}, {"touch", "Touchpad press"}, {"touch_left", "Touchpad left"},
    {"touch_right", "Touchpad right"}, {"swipe_up", "Swipe up"}, {"swipe_down", "Swipe down"},
    {"swipe_left", "Swipe left"}, {"swipe_right", "Swipe right"}};
constexpr int kSourceCount = int(std::size(kSources)), kTargetCount = int(std::size(kTargets));
// pad.c's default layout: the PS4 one, View / Menu the touchpad sides, Guide Options.
const int kDefaultMap[kSourceCount] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 18, 19, 11, 0, 13, 14, 15, 16, 0, 0, 0, 0};

void ParseMap(const char *text, int *map) {
    std::copy(kDefaultMap, kDefaultMap + kSourceCount, map);
    std::string s = text;
    size_t at = 0;
    while (at < s.size()) {
        size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        for (int i = 0; i < kSourceCount; ++i)
            if (item.compare(0, eq, kSources[i].key) == 0 && std::strlen(kSources[i].key) == eq)
                for (int t = 0; t < kTargetCount; ++t)
                    if (item.substr(eq + 1) == kTargets[t].key) map[i] = t;
    }
}
std::string FormatMap(const int *map) {
    if (std::equal(map, map + kSourceCount, kDefaultMap)) return "";
    std::string out;
    for (int i = 0; i < kSourceCount; ++i)
        if (map[i] != kDefaultMap[i]) out += std::string(out.empty() ? "" : ",") + kSources[i].key + "=" + kTargets[map[i]].key;
    return out;
}

// The first connected gamepad, opened on demand and dropped when it goes away.
SDL_Gamepad *Gamepad() {
    static SDL_Gamepad *pad = nullptr;
    if (pad && !SDL_GamepadConnected(pad)) { SDL_CloseGamepad(pad); pad = nullptr; }
    if (!pad) {
        int count = 0;
        if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
            if (count > 0) pad = SDL_OpenGamepad(ids[0]);
            SDL_free(ids);
        }
    }
    return pad;
}

// A dead zone slider with the stick's travel beside it: the ring is the full deflection, the red
// disc what the game ignores, the dot where the connected controller's stick is now.
void StickDeadZone(const char *label, const char *key, SDL_GamepadAxis axis_x, SDL_GamepadAxis axis_y) {
    Label(label);
    int dead = std::atoi(Value(key));
    if (dead < 0 || dead > 50) dead = 10;
    const float size = ImGui::GetFontSize() * 5;
    ImGui::SetNextItemWidth(-(size + ImGui::GetStyle().ItemSpacing.x));
    ImGui::PushID(key);
    if (ImGui::SliderInt("##dead", &dead, 0, 50, "%d%%")) Set(key, std::to_string(dead));
    ImGui::PopID();
    ImGui::SameLine();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 centre = {origin.x + size / 2, origin.y + size / 2};
    const float radius = size / 2 - 2;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(centre, radius, IM_COL32(255, 255, 255, 14), 64);
    draw->AddCircleFilled(centre, radius * dead / 100.0f, IM_COL32(204, 41, 51, 150), 48);
    draw->AddCircle(centre, radius, IM_COL32(255, 255, 255, 110), 64, 1.5f);
    draw->AddLine({centre.x - radius, centre.y}, {centre.x + radius, centre.y}, IM_COL32(255, 255, 255, 30));
    draw->AddLine({centre.x, centre.y - radius}, {centre.x, centre.y + radius}, IM_COL32(255, 255, 255, 30));
    if (SDL_Gamepad *pad = Gamepad()) {
        float x = SDL_GetGamepadAxis(pad, axis_x) / 32767.0f, y = SDL_GetGamepadAxis(pad, axis_y) / 32767.0f;
        const float m = std::sqrt(x * x + y * y);
        if (m > 1) { x /= m; y /= m; }
        const bool outside = m > dead / 100.0f;
        draw->AddCircleFilled({centre.x + x * radius, centre.y + y * radius}, 4,
                              outside ? IM_COL32(90, 220, 120, 255) : IM_COL32(255, 255, 255, 220), 16);
    }
    ImGui::Dummy({size, size});
}

// The keyboard and mouse as the game uses them (src/pad.c). Keys light up while they are pressed, and while Alt
// is held the mouse turns into the touchpad, as it does in game. Kinds: 0 move, 1 action, 2 power, 3 touchpad,
// 4 game window.
void KeyboardDiagram() {
    struct Key { const char *label, *what; float x, y, w; int kind; ImGuiKey key; int shape; }; // shape: the PlayStation face button, 1 cross 2 circle 3 square 4 triangle
    static const Key keys[] = {
        {"Esc", "Options", 0, 0, 1, 4, ImGuiKey_Escape}, {"F4", "aim", 2, 0, 1, 4, ImGuiKey_F4}, {"F5", "panel", 3, 0, 1, 4, ImGuiKey_F5},
        {"F6", "touchpad", 4, 0, 1, 4, ImGuiKey_F6}, {"F7", "graffiti", 5, 0, 1, 4, ImGuiKey_F7}, {"F9", "fps", 6.5f, 0, 1, 4, ImGuiKey_F9},
        {"1", "swipe up", 1, 1, 1, 3, ImGuiKey_1}, {"2", "down", 2, 1, 1, 3, ImGuiKey_2}, {"3", "left", 3, 1, 1, 3, ImGuiKey_3},
        {"4", "right", 4, 1, 1, 3, ImGuiKey_4},
        {"Tab", "", 0, 2, 1.5f, 3, ImGuiKey_Tab}, {"Q", "L1", 1.5f, 2, 1, 2, ImGuiKey_Q}, {"W", "forward", 2.5f, 2, 1, 0, ImGuiKey_W},
        {"E", "subdue", 3.5f, 2, 1, 1, ImGuiKey_E, 4}, {"R", "R1", 4.5f, 2, 1, 2, ImGuiKey_R}, {"T", "pad middle", 5.5f, 2, 1, 3, ImGuiKey_T},
        {"A", "left", 1.75f, 3, 1, 0, ImGuiKey_A}, {"S", "back", 2.75f, 3, 1, 0, ImGuiKey_S}, {"D", "right", 3.75f, 3, 1, 0, ImGuiKey_D},
        {"F", "melee", 4.75f, 3, 1, 1, ImGuiKey_F, 3}, {"G", "ultimate", 5.75f, 3, 1, 2, ImGuiKey_G},
        {"Shift", "dash", 0, 4, 2.25f, 1, ImGuiKey_LeftShift, 2}, {"Z", "pad left", 2.25f, 4, 1, 3, ImGuiKey_Z},
        {"X", "absorb", 3.25f, 4, 1, 3, ImGuiKey_X}, {"M", "R3", 8.25f, 4, 1, 4, ImGuiKey_M},
        {"Ctrl", "L3", 0, 5, 1.25f, 4, ImGuiKey_LeftCtrl}, {"Alt", "hold: touchpad", 2.5f, 5, 1.5f, 3, ImGuiKey_LeftAlt},
        {"Space", "jump", 4, 5, 6.25f, 1, ImGuiKey_Space, 1},
        {"^", "", 12.5f, 4, 1, 4, ImGuiKey_UpArrow}, {"<", "", 11.5f, 5, 1, 4, ImGuiKey_LeftArrow},
        {"v", "", 12.5f, 5, 1, 4, ImGuiKey_DownArrow}, {">", "", 13.5f, 5, 1, 4, ImGuiKey_RightArrow},
    };
    static const ImU32 fill[] = {IM_COL32(48, 92, 168, 255), IM_COL32(160, 52, 62, 255), IM_COL32(120, 82, 168, 255),
                                 IM_COL32(168, 128, 36, 255), IM_COL32(64, 68, 78, 255)};
    static const char *const kinds[] = {"movement", "actions", "powers", "touchpad", "game window"};
    const bool pad = ImGui::IsKeyDown(ImGuiKey_LeftAlt);
    const float width = ImGui::GetContentRegionAvail().x;
    const float u = std::min(width / 14.6f, ImGui::GetFontSize() * 3.4f), gap = u * 0.08f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    const float label_size = ImGui::GetFontSize() * 0.95f, what_size = ImGui::GetFontSize() * 0.72f;
    const ImU32 white = IM_COL32(245, 246, 248, 255), soft = IM_COL32(245, 246, 248, 215);
    const auto centred = [&](float size, ImVec2 a, ImVec2 b, float y, const char *text, ImU32 colour) {
        ImVec2 sz = font->CalcTextSizeA(size, 1e9f, 0.f, text);
        if (sz.x > b.x - a.x - 4) { size *= (b.x - a.x - 4) / sz.x; sz = font->CalcTextSizeA(size, 1e9f, 0.f, text); }
        draw->AddText(font, size, {(a.x + b.x - sz.x) / 2, y}, colour, text);
    };
    const auto lighter = [](ImU32 c) {
        const auto up = [](unsigned v) { return std::min(255u, v + 70u); };
        return IM_COL32(up(c & 255), up((c >> 8) & 255), up((c >> 16) & 255), 255);
    };
    for (const Key &k : keys) {
        const ImVec2 a = {origin.x + k.x * u, origin.y + k.y * u}, b = {a.x + k.w * u - gap, a.y + u - gap};
        const bool down = ImGui::IsKeyDown(k.key);
        draw->AddRectFilled(a, b, down ? lighter(fill[k.kind]) : fill[k.kind], u * 0.1f);
        if (down) draw->AddRect(a, b, white, u * 0.1f, 0, 2.f);
        const bool caption = *k.what != 0;
        centred(label_size, a, b, caption ? a.y + u * 0.08f : a.y + (b.y - a.y - label_size) / 2, k.label, white);
        if (caption) centred(what_size, a, b, b.y - what_size - u * 0.1f, k.what, soft);
        if (k.shape) {
            // A badge in the corner with the button's PlayStation shape, as the game's own prompts show it.
            static const ImU32 shape_colour[] = {0, IM_COL32(120, 170, 240, 255), IM_COL32(240, 100, 100, 255),
                                                 IM_COL32(240, 140, 200, 255), IM_COL32(90, 210, 150, 255)};
            const float r = u * 0.17f;
            const ImVec2 c = {b.x - u * 0.13f, a.y + u * 0.13f};
            const float h = r * 0.5f;
            draw->AddCircleFilled(c, r, IM_COL32(20, 22, 28, 235), 20);
            const ImU32 sc = shape_colour[k.shape];
            if (k.shape == 1) { draw->AddLine({c.x - h, c.y - h}, {c.x + h, c.y + h}, sc, 1.6f); draw->AddLine({c.x + h, c.y - h}, {c.x - h, c.y + h}, sc, 1.6f); }
            else if (k.shape == 2) draw->AddCircle(c, h * 1.05f, sc, 16, 1.6f);
            else if (k.shape == 3) draw->AddRect({c.x - h, c.y - h}, {c.x + h, c.y + h}, sc, 0, 0, 1.6f);
            else draw->AddTriangle({c.x, c.y - h * 1.15f}, {c.x + h * 1.1f, c.y + h * 0.85f}, {c.x - h * 1.1f, c.y + h * 0.85f}, sc, 1.6f);
        }
    }
    // The mouse: aiming it fires (left, R2) and aims (right, L2), the wheel is R3. Holding Alt makes it the touchpad.
    const ImVec2 m0 = {origin.x + 9.4f * u, origin.y}, m1 = {m0.x + 2.4f * u, m0.y + 3.5f * u};
    const ImU32 left_colour = pad ? fill[3] : fill[1], right_colour = pad ? fill[3] : fill[0];
    draw->AddRectFilled(m0, m1, IM_COL32(40, 43, 51, 255), u * 0.9f, ImDrawFlags_RoundCornersAll);
    draw->AddRect(m0, m1, pad ? fill[3] : IM_COL32(255, 255, 255, 70), u * 0.9f, ImDrawFlags_RoundCornersAll, pad ? 3.f : 1.5f);
    const float mid = (m0.x + m1.x) / 2, split = m0.y + 1.35f * u;
    const ImGuiIO &io = ImGui::GetIO();
    draw->AddRectFilled({m0.x + 2, m0.y + 2}, {mid - 1, split}, io.MouseDown[0] ? lighter(left_colour) : left_colour, u * 0.9f, ImDrawFlags_RoundCornersTopLeft);
    draw->AddRectFilled({mid + 1, m0.y + 2}, {m1.x - 2, split}, io.MouseDown[1] ? lighter(right_colour) : right_colour, u * 0.9f, ImDrawFlags_RoundCornersTopRight);
    draw->AddRectFilled({mid - u * 0.14f, m0.y + 0.3f * u}, {mid + u * 0.14f, m0.y + 0.95f * u}, io.MouseDown[2] ? lighter(left_colour) : white, u * 0.1f);
    centred(what_size * 1.25f, {m0.x, m0.y}, {mid, split}, m0.y + 1.0f * u, pad ? "finger" : "fire  R2", white);
    centred(what_size * 1.25f, {mid, m0.y}, {m1.x, split}, m0.y + 1.0f * u, pad ? "click" : "aim  L2", white);
    centred(what_size * 1.1f, {m0.x, split}, {m1.x, m1.y}, split + 0.4f * u, pad ? "wheel or side" : "look", soft);
    centred(what_size * 1.1f, {m0.x, split}, {m1.x, m1.y}, split + 0.9f * u, pad ? "button: R2" : "wheel: R3", soft);
    centred(what_size * 1.1f, {m0.x, m1.y}, {m1.x, m1.y + u}, m1.y + 0.1f * u, pad ? "TOUCHPAD" : "", fill[3]);
    ImGui::Dummy({width, u * 6.1f});
    // The legend.
    float x = 0;
    const float swatch = ImGui::GetFontSize() * 0.8f;
    for (int i = 0; i < 5; ++i) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        draw->AddRectFilled({at.x + x, at.y + 3}, {at.x + x + swatch, at.y + 3 + swatch}, fill[i], 3);
        draw->AddText({at.x + x + swatch + 6, at.y}, IM_COL32(210, 212, 218, 255), kinds[i]);
        x += swatch + 6 + ImGui::CalcTextSize(kinds[i]).x + ImGui::GetFontSize() * 1.2f;
    }
    ImGui::Dummy({width, ImGui::GetTextLineHeight() + 6});
    ImGui::Spacing();
}

void ControlsTab() {
    int map[kSourceCount];
    ParseMap(Value("pad_map"), map);
    ImGui::PushFont(g_heading, kHeading);
    ImGui::TextUnformatted("KEYBOARD AND MOUSE");
    ImGui::PopFont();
    KeyboardDiagram();
    ImGui::PushFont(g_heading, kHeading);
    ImGui::TextUnformatted("SETTINGS");
    ImGui::PopFont();
    if (!BeginForm("controls")) return;
    StickDeadZone("Left stick dead zone", "deadzone_left", SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY);
    StickDeadZone("Right stick dead zone", "deadzone_right", SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY);
    Combo("Mouse look and aim", "mouse_aim", kOnOff);
    Label("Mouse sensitivity");
    int sens = std::atoi(Value("mouse_sensitivity"));
    if (sens < 10 || sens > 1000) sens = 100;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##sens", &sens, 10, 1000, "%d%%", ImGuiSliderFlags_Logarithmic)) Set("mouse_sensitivity", std::to_string(sens));
    Combo("On-screen hints", "hints", {{"Auto (full the first times)", "auto"}, {"Always", "always"}, {"Off", "off"}});
    bool changed = false;
    for (int i = 0; i < kSourceCount; ++i) {
        Label(kSources[i].label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::PushID(i);
        if (ImGui::BeginCombo("##t", kTargets[map[i]].label)) {
            for (int t = 0; t < kTargetCount; ++t)
                if (ImGui::Selectable(kTargets[t].label, t == map[i])) { map[i] = t; changed = true; }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (ImGui::Button("Recommended layout")) { std::copy(kDefaultMap, kDefaultMap + kSourceCount, map); changed = true; }
    if (changed) Set("pad_map", FormatMap(map));
}

// Folder dialogs (xdg-desktop-portal or zenity through SDL). The answer may come on another thread.
std::mutex g_dialog_lock;
std::optional<std::pair<std::string, std::string>> g_dialog_result; // key, folder
void FolderCallback(void *key, const char *const *files, int) {
    if (!files || !files[0]) return;
    std::lock_guard lock(g_dialog_lock);
    g_dialog_result = {{static_cast<const char *>(key), files[0]}};
}
void FolderField(SDL_Window *window, const char *label, const char *key, const char *placeholder) {
    Label(label);
    ImGui::PushID(key);
    const float button = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(-button - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##path", placeholder, Value(key), SETTINGS_VALUE_SIZE);
    ImGui::SameLine();
    if (ImGui::Button("Browse")) {
        const std::string start = *Value(key) ? Value(key) : std::string(getenv("HOME") ? getenv("HOME") : "/");
        SDL_ShowOpenFolderDialog(FolderCallback, const_cast<char *>(key), window, start.c_str(), false);
    }
    ImGui::PopID();
}

void FoldersTab(SDL_Window *window) {
    if (BeginForm("folders")) {
        const std::string recorded = GameDir();
        FolderField(window, "Game folder", "game_dir", recorded.empty() ? "The folder with eboot.bin" : recorded.c_str());
        if (!recorded.empty() && !fs::exists(fs::path(recorded) / "eboot.bin")) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TableNextColumn();
            ImGui::TextColored({1, 0.4f, 0.4f, 1}, "No eboot.bin in this folder.");
        }
        const std::string saves = (UserDir() / "savedata").string();
        FolderField(window, "Save folder", "save_dir", saves.c_str());
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button("Open saves")) Open(SaveDir());
    ImGui::SameLine();
    if (ImGui::Button("Open log")) Open(LogFile());
    ImGui::SameLine();
    if (ImGui::Button("Open settings file")) Open(SettingsFile());
}

void Frame(SDL_Window *window, SDL_Renderer *renderer) {
    {
        std::lock_guard lock(g_dialog_lock);
        if (g_dialog_result) { Set(g_dialog_result->first.c_str(), g_dialog_result->second); g_dialog_result.reset(); }
    }
    LoadBackground(renderer);
    DrawBackground();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float margin = ImGui::GetFontSize() * 2;
    ImGui::SetNextWindowPos({margin, margin});
    ImGui::SetNextWindowSize({size.x - 2 * margin, size.y - 2 * margin});
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PushFont(g_title, kHeading * 2.2f);
    ImGui::TextUnformatted("inFAMOUS SECOND SON");
    ImGui::PopFont();
    ImGui::Spacing();

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f + ImGui::GetTextLineHeightWithSpacing();
    ImGui::PushFont(g_heading, kHeading);
    if (ImGui::BeginTabBar("tabs")) {
        const struct { const char *name; int id; } tabs[] = {{"DISPLAY", 0}, {"GRAPHICS", 1}, {"GAME", 2}, {"CONTROLS", 3}, {"FOLDERS", 4}};
        for (const auto &tab : tabs) {
            if (!ImGui::BeginTabItem(tab.name)) continue;
            ImGui::PushFont(g_body, kBody);
            ImGui::BeginChild("page", {0, -footer}, ImGuiChildFlags_NavFlattened);
            ImGui::Spacing();
            switch (tab.id) {
            case 0: DisplayTab(); break;
            case 1: GraphicsTab(); break;
            case 2: GameTab(); break;
            case 3: ControlsTab(); break;
            case 4: FoldersTab(window); break;
            }
            ImGui::EndChild();
            ImGui::PopFont();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::PushFont(g_body, kBody);
    if (!g_status.empty()) ImGui::TextColored(g_status_error ? ImVec4(1, 0.45f, 0.45f, 1) : ImVec4(0.7f, 0.9f, 0.7f, 1), "%s", g_status.c_str());
    ImGui::PopFont();
    const float play_w = ImGui::GetFontSize() * 9, play_h = ImGui::GetFrameHeight() * 1.6f;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.16f, 0.20f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.24f, 0.27f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.10f, 0.14f, 1));
    ImGui::PushFont(g_heading, kHeading * 1.4f);
    if (ImGui::Button("PLAY", {play_w, play_h})) Play(window);
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
    ImGui::SameLine();
    const float row = ImGui::GetCursorPosY() + (play_h - ImGui::GetFrameHeight()) / 2;
    ImGui::BeginDisabled(!Dirty());
    ImGui::SetCursorPosY(row);
    if (ImGui::Button("SAVE") && Save()) Status("Saved.");
    ImGui::SameLine();
    ImGui::SetCursorPosY(row);
    if (ImGui::Button("UNDO CHANGES")) { std::memcpy(g_values, g_saved, sizeof(g_values)); Status(""); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetCursorPosY(row);
    if (ImGui::Button("DEFAULTS"))
        for (int i = 0; i < settings_entry_count; ++i)
            if (std::strcmp(settings_entries[i].key, "game_dir") && std::strcmp(settings_entries[i].key, "save_dir"))
                std::snprintf(g_values[i], SETTINGS_VALUE_SIZE, "%s", settings_entries[i].fallback);
    ImGui::PopFont();
    ImGui::End();
}

} // namespace

int main(int, char **) {
    static_assert(sizeof(g_values) / sizeof(g_values[0]) >= 16);
    if (settings_entry_count > int(sizeof(g_values) / sizeof(g_values[0]))) return 1;
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return 1;
    exe[n] = 0;
    // ISS_ROOT: set by the AppImage, whose own folder is read-only.
    const char *root = getenv("ISS_ROOT");
    g_root = root && *root ? fs::path(root) : fs::path(exe).parent_path().parent_path();
    settings_load(g_values);
    std::memcpy(g_saved, g_values, sizeof(g_saved));
    // A missing file is written with the defaults now, so Open settings file has something to show.
    if (!fs::exists(SettingsFile())) settings_save(g_saved);

    SDL_SetHint(SDL_HINT_APP_ID, "iss-launcher");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()) > 0 ? SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()) : 1;
    SDL_Window *window = SDL_CreateWindow("inFAMOUS Second Son", int(860 * scale), int(680 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) { std::fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowMinimumSize(window, int(760 * scale), int(520 * scale));
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    {
        const fs::path icon = fs::path(GameDir()) / "sce_sys" / "icon0.png";
        int w, h, channels;
        if (unsigned char *pixels = stbi_load(icon.c_str(), &w, &h, &channels, 4)) {
            if (SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, pixels, w * 4)) {
                SDL_SetWindowIcon(window, surface);
                SDL_DestroySurface(surface);
            }
            stbi_image_free(pixels);
        }
    }
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    Style(scale);
    LoadFonts();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    bool running = true;
    while (running) {
        SDL_Event event;
        // While the game runs the window is hidden: wake up now and then to see whether it ended.
        if (SDL_WaitEventTimeout(&event, g_game > 0 ? 250 : 100)) {
            do {
                if (g_game > 0) continue;
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT ||
                    (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)))
                    running = false;
            } while (SDL_PollEvent(&event));
        }
        Reap(window);
        if (g_game > 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) continue;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        Frame(window, renderer);
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
