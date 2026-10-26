// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game menu (F1): settings that switch while playing and a live debug view, drawn over the
// game window with ImGui's draw list like the touchpad panel (pad_panel_ui.cpp). GRAPHICS: FSR 3
// anti-aliasing, the render size (fsr_mode, taken at the next start), sharpening and frame
// generation (fsr_upscale.h); a change is saved to settings.toml by
// src/pad.c, which takes it through iss_gpu_menu_take. DEBUG: real and shown frame rates, what
// FSR and FG did, the last hitch. While it is up src/pad.c frees the mouse (iss_gpu_menu_shown).
#include <SDL3/SDL.h>
#include "settings_value.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "core/debug_state.h"
#include "fsr_upscale.h"

namespace iss::save_ui { ImFont* UiFont(bool bold); }
namespace iss::menu_ui {
namespace {
enum { B_CLOSE, B_FSR, B_MODE, B_SHARP_DOWN, B_SHARP_UP, B_FG, B_COUNT };
constexpr int ROWS = 4;
constexpr float W = 460, TITLE = 30, ROW = 38, H = 444 + (ROWS - 2) * ROW;
// fsr_mode: settings.toml value, label, the output size divided by.
struct Mode { const char* key; const char* label; double scale; };
constexpr Mode kModes[] = {{"native", "Native", 1.0}, {"quality", "Quality", 1.5},
                           {"balanced", "Balanced", 1.7}, {"performance", "Performance", 2.0},
                           {"ultra_performance", "Ultra perf.", 3.0}};
constexpr int kModeCount = int(sizeof(kModes) / sizeof(kModes[0]));
int StartMode() {
    const char* v = iss::Setting("fsr_mode");
    const double scale = v && *v ? std::atof(v) : 1.0;
    for (int i = 0; i < kModeCount; ++i)
        if (std::fabs(kModes[i].scale - scale) < 0.01) return i;
    return 0;
}

struct Rect { float l, t, r, b; };
struct State {
    std::mutex mutex;
    bool shown = false, placed = false, dragging = false;
    int pressed = -1, hover = -1;
    float k = 1, ox = 0, oy = 0, drag_x = 0, drag_y = 0;
    std::vector<std::pair<std::string, std::string>> changes; // settings.toml key, value
    int start_mode = StartMode(), mode = start_mode; // the render size the game runs with, chosen
    // The rates, sampled every half second.
    u64 sample_ns = 0, flips = 0, presents = 0, generated = 0;
    double real_fps = 0, shown_fps = 0, generated_fps = 0;
};
State& S() { static State s; return s; }

Rect base_rect(int b) {
    switch (b) {
    case B_CLOSE: return {W - 36, 3, W - 6, TITLE - 3};
    case B_FSR: return {W - 112, 74, W - 12, 74 + ROW - 8};
    case B_MODE: return {W - 152, 74 + ROW, W - 12, 74 + 2 * ROW - 8};
    case B_SHARP_DOWN: return {W - 152, 74 + 2 * ROW, W - 112, 74 + 3 * ROW - 8};
    case B_SHARP_UP: return {W - 52, 74 + 2 * ROW, W - 12, 74 + 3 * ROW - 8};
    default: return {W - 112, 74 + 3 * ROW, W - 12, 74 + 4 * ROW - 8};
    }
}
Rect screen_rect(const State& s, Rect r) { return {s.ox + r.l * s.k, s.oy + r.t * s.k, s.ox + r.r * s.k, s.oy + r.b * s.k}; }
bool inside(Rect r, float x, float y) { return x >= r.l && x < r.r && y >= r.t && y < r.b; }
Rect panel_rect(const State& s) { return {s.ox, s.oy, s.ox + W * s.k, s.oy + H * s.k}; }
int hit_button(const State& s, float x, float y) {
    for (int b = 0; b < B_COUNT; ++b)
        if (inside(screen_rect(s, base_rect(b)), x, y)) return b;
    return -1;
}
// Window coordinates to the drawable's pixels (the draw list's), as the touchpad panel does.
void to_pixels(SDL_WindowID id, float& x, float& y) {
    if (SDL_Window* w = SDL_GetWindowFromID(id)) {
        int pw, ph, ww, wh;
        SDL_GetWindowSizeInPixels(w, &pw, &ph);
        SDL_GetWindowSize(w, &ww, &wh);
        if (ww > 0 && wh > 0) { x *= float(pw) / ww; y *= float(ph) / wh; }
    }
}
void press(State& s, int b) {
    switch (b) {
    case B_CLOSE: s.shown = false; break;
    case B_FSR: {
        const bool on = !iss::fsr::Enabled();
        iss::fsr::SetEnabled(on);
        s.changes.emplace_back("fsr_aa", on ? "on" : "off");
        break;
    }
    case B_MODE:
        s.mode = (s.mode + 1) % kModeCount;
        s.changes.emplace_back("fsr_mode", kModes[s.mode].key);
        break;
    case B_SHARP_DOWN:
    case B_SHARP_UP: {
        const int now = iss::fsr::Sharpness();
        const int next = std::clamp(now / 10 * 10 + (b == B_SHARP_UP ? 10 : now % 10 ? 0 : -10), 0, 100);
        if (next == now) break;
        iss::fsr::SetSharpness(next);
        s.changes.emplace_back("fsr_sharpness", std::to_string(next));
        break;
    }
    case B_FG: {
        if (!iss::fsr::Enabled()) break; // FG needs FSR
        const bool on = !iss::fsr::FgEnabled();
        iss::fsr::SetFgEnabled(on);
        s.changes.emplace_back("frame_generation", on ? "on" : "off");
        break;
    }
    default: break;
    }
}
ImU32 C(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
} // namespace

bool Shown() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    return s.shown;
}

// F1 shows and hides it; while it is up the mouse is the menu's on the panel (and drags it by
// the title bar). True: the event is taken.
bool OnEvent(const SDL_Event& e) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F1) {
        s.shown = !s.shown;
        s.pressed = -1;
        s.dragging = false;
        std::fprintf(stderr, "MENU visible=%d\n", int(s.shown));
        return true;
    }
    if (!s.shown) return false;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        float x = e.motion.x, y = e.motion.y;
        to_pixels(e.motion.windowID, x, y);
        if (s.dragging) { s.ox = x - s.drag_x; s.oy = y - s.drag_y; }
        s.hover = hit_button(s, x, y);
        return inside(panel_rect(s), x, y) || s.dragging;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        float x = e.button.x, y = e.button.y;
        to_pixels(e.button.windowID, x, y);
        const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (!down) {
            const bool was = s.dragging || s.pressed >= 0;
            if (s.pressed >= 0 && hit_button(s, x, y) == s.pressed) press(s, s.pressed);
            s.pressed = -1;
            s.dragging = false;
            return was || inside(panel_rect(s), x, y);
        }
        if (!inside(panel_rect(s), x, y) || e.button.button != SDL_BUTTON_LEFT) return inside(panel_rect(s), x, y);
        s.pressed = hit_button(s, x, y);
        if (s.pressed < 0 && y < s.oy + TITLE * s.k) { s.dragging = true; s.drag_x = x - s.ox; s.drag_y = y - s.oy; }
        return true;
    }
    return false;
}

void Draw(float width, float height) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    auto& stats = iss::fsr::GetStats();
    // Rates over half a second, whether shown or not (the first look is already filled in).
    const u64 now = SDL_GetTicksNS();
    const u64 flips = DebugState.flip_count.load(std::memory_order_relaxed);
    const u64 presents = stats.presents.load(std::memory_order_relaxed);
    const u64 generated = stats.fg_generated.load(std::memory_order_relaxed);
    if (now - s.sample_ns >= 500000000ull) {
        const double sec = s.sample_ns ? double(now - s.sample_ns) / 1e9 : 0;
        if (sec > 0) {
            s.real_fps = double(flips - s.flips) / sec;
            s.shown_fps = double(presents - s.presents) / sec;
            s.generated_fps = double(generated - s.generated) / sec;
        }
        s.sample_ns = now; s.flips = flips; s.presents = presents; s.generated = generated;
    }
    if (!s.shown) return;
    s.k = std::clamp(height / 1200.f, 0.7f, 1.6f);
    const float k = s.k;
    if (!s.placed) { s.ox = 28 * k; s.oy = 56 * k; s.placed = true; }
    s.ox = std::clamp(s.ox, 0.f, std::max(0.f, width - W * k));
    s.oy = std::clamp(s.oy, 0.f, std::max(0.f, height - H * k));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = iss::save_ui::UiFont(false);
    if (!font) font = ImGui::GetFont();
    const float fs = 14 * k, small = 12.5f * k;
    const ImU32 blue = C(0, 112, 209), fg = C(232, 234, 238), dim = C(150, 156, 168), off = C(42, 46, 54);
    const Rect all = panel_rect(s);
    dl->AddRectFilled({all.l, all.t}, {all.r, all.b}, C(22, 24, 29, 240), 6 * k);
    dl->AddRectFilled({all.l, all.t}, {all.r, all.t + TITLE * k}, C(14, 15, 18), 6 * k, ImDrawFlags_RoundCornersTop);
    dl->AddRect({all.l, all.t}, {all.r, all.b}, C(255, 255, 255, 40), 6 * k);
    dl->AddText(font, fs, {all.l + 12 * k, all.t + (TITLE * k - fs) / 2}, fg, "MENU");
    dl->AddText(font, small, {all.l + 70 * k, all.t + (TITLE * k - small) / 2}, dim, "F1 shows / hides");
    const auto at = [&](float x, float y) { return ImVec2(s.ox + x * k, s.oy + y * k); };
    const auto heading = [&](float y, const char* t) {
        dl->AddText(font, small, at(12, y), dim, t);
        dl->AddLine(at(12, y + 18), at(W - 12, y + 18), C(58, 63, 74));
    };

    heading(44, "GRAPHICS");
    const bool fsr = iss::fsr::Enabled(), fgen = iss::fsr::FgEnabled();
    const char* names[B_COUNT] = {"", "FSR 3 anti-aliasing", "Render size", "Sharpening (FSR 3)", "",
                                  "Frame generation (FSR 3)"};
    const auto centered = [&](Rect r, ImU32 c, const char* t) {
        const ImVec2 sz = font->CalcTextSizeA(fs, 1e9f, 0.f, t);
        dl->AddText(font, fs, {(r.l + r.r - sz.x) / 2, (r.t + r.b - sz.y) / 2}, c, t);
    };
    for (int b = 0; b < B_COUNT; ++b) {
        const Rect r = screen_rect(s, base_rect(b));
        const bool hot = s.pressed == b || s.hover == b;
        if (b == B_MODE) {
            dl->AddText(font, fs, {s.ox + 12 * k, (r.t + r.b - fs) / 2}, fg, names[b]);
            const auto [gw, gh] = DebugState.game_resolution;
            char note[64];
            if (s.mode != s.start_mode)
                std::snprintf(note, sizeof(note), "after a restart");
            else
                std::snprintf(note, sizeof(note), "%ux%u", gw, gh);
            dl->AddText(font, small, {s.ox + 120 * k, (r.t + r.b - small) / 2}, dim, note);
            dl->AddRectFilled({r.l, r.t}, {r.r, r.b}, hot ? C(58, 63, 74) : off, 8 * k);
            centered(r, fg, kModes[s.mode].label);
            continue;
        }
        if (b == B_SHARP_DOWN || b == B_SHARP_UP) {
            if (b == B_SHARP_DOWN) {
                dl->AddText(font, fs, {s.ox + 12 * k, (r.t + r.b - fs) / 2}, fsr ? fg : dim, names[B_SHARP_DOWN]);
                const int sharpness = iss::fsr::Sharpness();
                char value[16];
                std::snprintf(value, sizeof(value), sharpness ? "%d%%" : "off", sharpness);
                centered({r.r, r.t, r.r + 60 * k, r.b}, fsr ? fg : dim, value);
            }
            dl->AddRectFilled({r.l, r.t}, {r.r, r.b}, hot ? C(58, 63, 74) : off, 8 * k);
            centered(r, fg, b == B_SHARP_DOWN ? "-" : "+");
            continue;
        }
        if (b == B_CLOSE) {
            dl->AddRectFilled({r.l, r.t}, {r.r, r.b}, s.pressed == b || s.hover == b ? C(196, 43, 28) : C(14, 15, 18));
            const ImVec2 sz = font->CalcTextSizeA(fs, 1e9f, 0.f, "X");
            dl->AddText(font, fs, {(r.l + r.r - sz.x) / 2, (r.t + r.b - sz.y) / 2}, fg, "X");
            continue;
        }
        const bool on = b == B_FSR ? fsr : fgen, usable = b == B_FSR || fsr;
        dl->AddText(font, fs, {s.ox + 12 * k, (r.t + r.b - fs) / 2}, usable ? fg : dim, names[b]);
        ImU32 c = on ? blue : off;
        if (usable && (s.pressed == b || s.hover == b)) c = on ? C(30, 136, 229) : C(58, 63, 74);
        dl->AddRectFilled({r.l, r.t}, {r.r, r.b}, c, 8 * k);
        const char* label = !usable ? "needs FSR" : on ? "ON" : "OFF";
        const ImVec2 sz = font->CalcTextSizeA(fs, 1e9f, 0.f, label);
        dl->AddText(font, fs, {(r.l + r.r - sz.x) / 2, (r.t + r.b - sz.y) / 2}, usable ? fg : dim, label);
    }

    heading(74 + ROWS * ROW + 6, "DEBUG");
    const auto [gw, gh] = DebugState.game_resolution;
    const auto [ow, oh] = DebugState.output_resolution;
    const u64 fsr_done = stats.fsr_resolves.load(), game_done = stats.game_resolves.load();
    const u64 skipped = stats.fg_skipped.load();
    const u32 hitch = stats.last_hitch_ms.load();
    const u64 hitches = stats.hitches.load(), doubles = stats.double_renders.load();
    char lines[9][160];
    std::snprintf(lines[0], sizeof(lines[0]), "Game FPS  %.1f  (%.1f ms)", s.real_fps, s.real_fps > 0 ? 1000 / s.real_fps : 0.0);
    std::snprintf(lines[1], sizeof(lines[1]), "Shown FPS  %.1f   generated %.1f/s", s.shown_fps, s.generated_fps);
    std::snprintf(lines[2], sizeof(lines[2]), "FSR  %s   resolves %llu, kept the game's %llu",
                  fsr ? "on" : "off", (unsigned long long)fsr_done, (unsigned long long)game_done);
    std::snprintf(lines[3], sizeof(lines[3]), "FG  %s   generated %llu, skipped %llu",
                  fgen ? "on" : "off", (unsigned long long)generated, (unsigned long long)skipped);
    std::snprintf(lines[4], sizeof(lines[4]), "Hitches %llu, last %s", (unsigned long long)hitches,
                  hitch ? (std::to_string(hitch) + " ms").c_str() : "none");
    std::snprintf(lines[5], sizeof(lines[5]), "Scene rendered twice per frame  %llu", (unsigned long long)doubles);
    std::snprintf(lines[6], sizeof(lines[6]), "Picture %ux%u  ->  window %ux%u", gw, gh, ow, oh);
    std::snprintf(lines[7], sizeof(lines[7]), "FG skips a frame after a hitch (over 2.5x the usual frame),");
    std::snprintf(lines[8], sizeof(lines[8]), "a frame rendered twice, a menu or loading.");
    float y = 74 + ROWS * ROW + 32;
    for (int i = 0; i < 9; ++i, y += 22)
        dl->AddText(font, i >= 7 ? small : fs, at(12, y), i >= 7 ? dim : fg, lines[i]);
}
} // namespace iss::menu_ui

// For src/pad.c: the menu is up (the mouse is freed for it).
extern "C" __declspec(dllexport) int iss_gpu_menu_shown(void) {
    return iss::menu_ui::Shown() ? 1 : 0;
}
// For src/pad.c: one setting the menu changed, to save in settings.toml. 0: none.
extern "C" __declspec(dllexport) int iss_gpu_menu_take(char* key, size_t key_size,
                                                                         char* value, size_t value_size) {
    auto& s = iss::menu_ui::S();
    std::lock_guard lock{s.mutex};
    if (s.changes.empty()) return 0;
    std::snprintf(key, key_size, "%s", s.changes.front().first.c_str());
    std::snprintf(value, value_size, "%s", s.changes.front().second.c_str());
    s.changes.erase(s.changes.begin());
    return 1;
}
