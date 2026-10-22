// SPDX-License-Identifier: GPL-2.0-or-later
// The touchpad and spray panel (F5 in game), drawn over the game window with ImGui's draw list
// like the save dialog: it works the same on X11, Wayland and fullscreen. The runtime (src/pad.c)
// reads it through iss_gpu_pad_panel_*. TOUCHPAD: a pad to press and drag with the mouse, the
// sides, click and swipes of PhonePad Desktop, and the mouse as touchpad (F6). SPRAY: the spray
// can of the graffiti missions (F7), its speed and a guide.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include "../src/pad_panel.h"

namespace iss::save_ui { ImFont* UiFont(bool bold); }
namespace iss::pad_ui {
namespace {
enum { B_TAB_TOUCH, B_TAB_SPRAY, B_CLOSE, B_LEFT, B_BOTH, B_RIGHT, B_CLICK, B_UP, B_DOWN, B_SWLEFT, B_SWRIGHT,
       B_MOUSE, B_OPTIONS, B_SPRAY, B_RECENTRE, B_LESS, B_MORE, B_COUNT };
const char* const labels[B_COUNT] = {"TOUCHPAD", "SPRAY", "X", "Left", "Both", "Right", "Click", "Up  Swipe",
    "Down  Swipe", "Left  Swipe", "Right  Swipe", "Mouse touchpad   (F6)", "Options (pause)", "START SPRAY MODE   (F7)",
    "Recenter", "-", "+"};
const char* const touch_guide =
    "Drag on the pad: a finger.  Right button: click.  Shift: two fingers.\n"
    "Left / Right / Both: press that side of the touchpad (the game's touchpad prompts). Click: the middle.\n"
    "Swipes: the game's swipe prompts.\n"
    "Mouse as touchpad (F6): in the game window the left button is a finger, the right button clicks.\n"
    "Xbox / other gamepad: View and Menu are the left and right side, Guide is Options (pause). Hold L3 + R3: the right stick is the finger, "
    "the D-pad swipes, A clicks.";
const char* const spray_guide =
    "For the graffiti missions:\n\n"
    "1.  At a graffiti spot, press START SPRAY MODE (or F7). The controller is held like a spray can "
    "and the cursor is hidden in the game window.\n\n"
    "2.  Shake the can: hold the right mouse button.\n\n"
    "3.  Spray: hold the left mouse button and move the mouse to paint. Shift: fine aim. "
    "Middle button: recenter.\n\n"
    "4.  Press F7 to end spray mode and get the cursor back.";
constexpr float W = 420, H = 660, TITLE = 30, PAD_X = 12, PAD_Y = 74, PAD_W = 396, PAD_H = 194; // the pad is 1920:942

struct Rect { float l, t, r, b; };
struct State {
    std::mutex mutex;
    bool shown = false, placed = false, dragging = false, touch = false, over = false;
    int mode = 0, tab = 0, pressed = -1, held = 0;
    float k = 0, ox = 0, oy = 0, px = -1, py = -1, drag_x = 0, drag_y = 0;
    PadPanel out{};
    PadLive live{};
    State() {
        out.swipe = out.mode = -1; out.sensitivity = 1;
        if (const char* p = std::getenv("ISS_PAD_PANEL")) shown = p[0] == '1'; // starts open (checks and screenshots)
    }
};
State& S() { static State s; return s; }

bool visible(const State& s, int b) { return b <= B_CLOSE || (s.tab == 0 ? b >= B_LEFT && b <= B_OPTIONS : b >= B_SPRAY); }
Rect base_rect(int b) {
    switch (b) {
    case B_TAB_TOUCH: return {12, 38, 206, 66};
    case B_TAB_SPRAY: return {214, 38, 408, 66};
    case B_CLOSE: return {W - 36, 3, W - 6, TITLE - 3};
    case B_MOUSE: return {12, 354, 206, 384};
    case B_OPTIONS: return {214, 354, 408, 384};
    case B_SPRAY: return {12, 74, 408, 124};
    case B_RECENTRE: return {12, 134, 206, 164};
    case B_LESS: return {214, 134, 254, 164};
    case B_MORE: return {368, 134, 408, 164};
    default: {
        const int i = b - B_LEFT, row = i / 4, col = i % 4;
        const float l = PAD_X + col * 101, t = 278.f + row * 38;
        return {l, t, l + 93, t + 30};
    }
    }
}
Rect screen_rect(const State& s, Rect r) { return {s.ox + r.l * s.k, s.oy + r.t * s.k, s.ox + r.r * s.k, s.oy + r.b * s.k}; }
bool inside(Rect r, float x, float y) { return x >= r.l && x < r.r && y >= r.t && y < r.b; }
Rect panel_rect(const State& s) { return {s.ox, s.oy, s.ox + W * s.k, s.oy + H * s.k}; }
Rect pad_rect(const State& s) { return screen_rect(s, {PAD_X, PAD_Y, PAD_X + PAD_W, PAD_Y + PAD_H}); }
int hit_button(const State& s, float x, float y) {
    for (int b = 0; b < B_COUNT; ++b)
        if (visible(s, b) && inside(screen_rect(s, base_rect(b)), x, y)) return b;
    return -1;
}
void set_finger(State& s, float x, float y) {
    const Rect r = pad_rect(s);
    s.out.x = std::clamp((x - r.l) / (r.r - r.l), 0.f, 1.f);
    s.out.y = std::clamp((y - r.t) / (r.b - r.t), 0.f, 1.f);
    s.out.two = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
}
void release_all(State& s) {
    s.out.touch = s.out.two = s.out.click = s.out.left = s.out.right = s.out.both = s.out.centre = s.out.options = 0;
    s.pressed = -1; s.touch = false; s.held = 0; s.dragging = false;
}
void press(State& s, int b, bool down) {
    PadPanel& o = s.out;
    switch (b) {
    case B_LEFT: o.left = down; break;
    case B_BOTH: o.both = down; break;
    case B_RIGHT: o.right = down; break;
    case B_CLICK: o.centre = down; break;
    case B_OPTIONS: o.options = down; break;
    case B_UP: case B_DOWN: case B_SWLEFT: case B_SWRIGHT: if (down) o.swipe = b - B_UP; break;
    case B_RECENTRE: if (down) o.recentre = 1; break;
    case B_SPRAY: if (down) o.mode = 2; break;
    case B_MOUSE: if (down) o.mode = 1; break;
    case B_LESS: if (down && o.sensitivity > .25f) o.sensitivity -= .1f; break;
    case B_MORE: if (down && o.sensitivity < 1.95f) o.sensitivity += .1f; break;
    case B_TAB_TOUCH: if (down) s.tab = 0; break;
    case B_TAB_SPRAY: if (down) s.tab = 1; break;
    default: break;
    }
}
void pointer_from(const SDL_Event& e, float x, float y, State& s) {
    float sx = 1, sy = 1;
    SDL_WindowID id = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.windowID : e.button.windowID;
    if (SDL_Window* w = SDL_GetWindowFromID(id)) {
        int pw, ph, ww, wh;
        SDL_GetWindowSizeInPixels(w, &pw, &ph);
        SDL_GetWindowSize(w, &ww, &wh);
        if (ww > 0 && wh > 0) { sx = float(pw) / ww; sy = float(ph) / wh; }
    }
    s.px = x * sx; s.py = y * sy;
}
ImU32 C(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
}

constexpr float LIVE_Y = 520; // the controller view: y .. H - 8
// The physical controller: buttons, sticks, triggers and the fingers the game gets on the touchpad.
void DrawLive(ImDrawList* dl, ImFont* font, const State& s, float small, float alpha) {
    const float k = s.k;
    const PadLive& v = s.live;
    auto P = [&](float x, float y) { return ImVec2(s.ox + x * k, s.oy + (LIVE_Y + y) * k); };
    auto col = [&](int r, int g, int b) { return C(r, g, b, int(255 * alpha)); };
    const ImU32 off = col(42, 46, 54), on = col(0, 112, 209), dim = col(150, 156, 168), ring = col(58, 63, 74);
    dl->AddRectFilled(P(12, 0), P(W - 12, H - LIVE_Y - 8), col(27, 29, 35), 8 * k);
    const char* title = v.connected ? "CONTROLLER" : "CONTROLLER  -  none connected (keyboard / mouse only)";
    dl->AddText(font, small, P(22, 4), dim, title);
    if (!v.connected) return;
    auto lit = [&](int bit) { return (v.sources >> bit & 1) != 0; };
    auto box = [&](float x0, float y0, float x1, float y1, bool down, const char* t) {
        dl->AddRectFilled(P(x0, y0), P(x1, y1), down ? on : off, 4 * k);
        if (t) { const ImVec2 sz = font->CalcTextSizeA(small, 1e9f, 0.f, t);
            const ImVec2 a = P((x0 + x1) / 2, (y0 + y1) / 2);
            dl->AddText(font, small, ImVec2(a.x - sz.x / 2, a.y - sz.y / 2), col(232, 234, 238), t); }
    };
    // Triggers (analog) and bumpers.
    for (int side = 0; side < 2; ++side) {
        const float x0 = side ? 296 : 24, x1 = x0 + 100;
        const float amount = (side ? v.rt : v.lt) / 255.f;
        dl->AddRectFilled(P(x0, 24), P(x1, 34), off, 3 * k);
        dl->AddRectFilled(P(x0, 24), P(x0 + 100 * amount, 34), on, 3 * k);
        box(x0, 38, x1, 52, lit(side ? 5 : 4), side ? "RB" : "LB");
    }
    // D-pad and face buttons.
    const float dx = 58, dy = 92, fx = 362 - 24, fy = 92;
    box(dx - 8, dy - 24, dx + 8, dy - 8, lit(14), nullptr);
    box(dx - 8, dy + 8, dx + 8, dy + 24, lit(15), nullptr);
    box(dx - 24, dy - 8, dx - 8, dy + 8, lit(16), nullptr);
    box(dx + 8, dy - 8, dx + 24, dy + 8, lit(17), nullptr);
    const struct { float x, y; int bit; const char* t; } face[4] = {{0, -20, 3, "Y"}, {0, 20, 0, "A"}, {-20, 0, 2, "X"}, {20, 0, 1, "B"}};
    for (const auto& f : face) {
        const ImVec2 c = P(fx + f.x, fy + f.y);
        dl->AddCircleFilled(c, 9 * k, lit(f.bit) ? on : off);
        const ImVec2 sz = font->CalcTextSizeA(small, 1e9f, 0.f, f.t);
        dl->AddText(font, small, ImVec2(c.x - sz.x / 2, c.y - sz.y / 2), col(232, 234, 238), f.t);
    }
    // Sticks: the dot is the deflection, the ring lights while the stick is clicked.
    const struct { float x, y; uint8_t ax, ay; int bit; } sticks[2] = {{140, 100, v.lx, v.ly, 8}, {280, 100, v.rx, v.ry, 9}};
    for (const auto& st : sticks) {
        const ImVec2 c = P(st.x, st.y);
        dl->AddCircleFilled(c, 24 * k, col(35, 38, 45));
        dl->AddCircle(c, 24 * k, lit(st.bit) ? on : ring, 0, 2 * k);
        dl->AddCircleFilled(ImVec2(c.x + (st.ax - 128) / 128.f * 17 * k, c.y + (st.ay - 128) / 128.f * 17 * k), 6 * k, lit(st.bit) ? on : col(232, 234, 238));
    }
    // View / Menu, and a small touchpad with the fingers the game gets.
    box(110, 58, 154, 74, lit(10), "View");
    box(266, 58, 310, 74, lit(11), "Menu");
    const ImVec2 a = P(158, 56), b = P(262, 90);
    dl->AddRectFilled(a, b, v.click ? col(30, 50, 78) : col(35, 38, 45), 5 * k);
    dl->AddLine({(a.x + b.x) / 2, a.y + 3 * k}, {(a.x + b.x) / 2, b.y - 3 * k}, ring);
    for (int i = 0; i < 2; ++i)
        if (v.contacts[i])
            dl->AddCircleFilled({a.x + v.xy[i][0] / 1919.f * (b.x - a.x), a.y + v.xy[i][1] / 941.f * (b.y - a.y)}, 4 * k, col(246, 190, 30));
}

// Mouse events of the game window (window thread): taken only while the pointer is on the panel
// or a press that began on it is still held. Keys are never taken (src/pad.c polls the keyboard).
bool OnEvent(const SDL_Event& e) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    if (!s.shown || s.mode == 2 || s.k <= 0) return false;
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        pointer_from(e, e.motion.x, e.motion.y, s);
        if (s.dragging) { s.ox += s.px - s.drag_x; s.oy += s.py - s.drag_y; s.drag_x = s.px; s.drag_y = s.py; }
        if (s.touch) set_finger(s, s.px, s.py);
        s.over = inside(panel_rect(s), s.px, s.py);
        return s.over || s.touch || s.dragging || s.pressed >= 0;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_RIGHT) return s.over;
        pointer_from(e, e.button.x, e.button.y, s);
        s.over = inside(panel_rect(s), s.px, s.py);
        if (!s.over) return false;
        ++s.held;
        const bool left = e.button.button == SDL_BUTTON_LEFT;
        if (s.tab == 0 && inside(pad_rect(s), s.px, s.py)) {
            s.touch = true; s.out.touch = 1; set_finger(s, s.px, s.py);
            if (!left) s.out.click = 1;
        } else if (left) {
            const int b = hit_button(s, s.px, s.py);
            if (b == B_CLOSE) { s.shown = false; release_all(s); return true; }
            if (b >= 0) { press(s, b, true); s.pressed = b; }
            else if (s.py < s.oy + TITLE * s.k) { s.dragging = true; s.drag_x = s.px; s.drag_y = s.py; }
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_RIGHT) return false;
        const bool active = s.held > 0;
        if (!active) return false;
        --s.held;
        if (e.button.button == SDL_BUTTON_RIGHT) s.out.click = 0;
        if (e.button.button == SDL_BUTTON_LEFT) {
            if (s.pressed >= 0) press(s, s.pressed, false);
            s.pressed = -1; s.dragging = false;
        }
        if (s.held <= 0) { s.held = 0; s.touch = false; s.out.touch = s.out.two = s.out.click = 0; }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: return s.over;
    default: return false;
    }
}

bool Shown() { State& s = S(); std::lock_guard lock{s.mutex}; return s.shown; }

// Every presented frame (render thread), before ImGui::Render.
void Draw(float width, float height) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    if (!s.shown) return;
    s.k = std::clamp(height / 1200.f, 0.7f, 1.6f);
    const float k = s.k;
    if (!s.placed) { s.ox = width - (W + 28) * k; s.oy = 56 * k; s.placed = true; }
    s.ox = std::clamp(s.ox, 0.f, std::max(0.f, width - W * k));
    s.oy = std::clamp(s.oy, 0.f, std::max(0.f, height - H * k));
    const float alpha = s.mode == 2 ? 0.84f : 1.f;
    auto col = [&](int r, int g, int b) { return C(r, g, b, int(255 * alpha)); };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = iss::save_ui::UiFont(false);
    if (!font) font = ImGui::GetFont();
    const float fs = 14 * k, small = 12.5f * k;
    const auto text_size = [&](const char* t, float size) { return font->CalcTextSizeA(size, 1e9f, 0.f, t); };
    const auto centered = [&](Rect r, const char* t, ImU32 c, float size) {
        const ImVec2 sz = text_size(t, size);
        dl->AddText(font, size, ImVec2((r.l + r.r - sz.x) / 2, (r.t + r.b - sz.y) / 2), c, t);
    };
    const Rect all = panel_rect(s);
    const ImU32 blue = col(0, 112, 209), fg = col(232, 234, 238), dim = col(150, 156, 168);
    dl->AddRectFilled({all.l, all.t}, {all.r, all.b}, col(22, 24, 29), 6 * k);
    dl->AddRectFilled({all.l, all.t}, {all.r, all.t + TITLE * k}, col(14, 15, 18), 6 * k, ImDrawFlags_RoundCornersTop);
    dl->AddRect({all.l, all.t}, {all.r, all.b}, C(255, 255, 255, int(40 * alpha)), 6 * k);
    dl->AddText(font, fs, {all.l + 12 * k, all.t + (TITLE * k - fs) / 2}, fg, "TOUCHPAD & SPRAY");
    dl->AddText(font, small, {all.l + 190 * k, all.t + (TITLE * k - small) / 2}, dim, "F5 shows / hides");
    const PadPanel& o = s.out;
    if (s.tab == 0) {
        const Rect pad = pad_rect(s);
        dl->AddRectFilled({pad.l, pad.t}, {pad.r, pad.b}, o.click ? col(30, 50, 78) : col(35, 38, 45), 14 * k);
        const float mid = (pad.l + pad.r) / 2;
        for (float y = pad.t + 10 * k; y < pad.b - 10 * k; y += 6 * k) dl->AddLine({mid, y}, {mid, y + 2 * k}, col(58, 63, 74));
        if (s.live.connected)
            for (int i = 0; i < 2; ++i)
                if (s.live.contacts[i])
                    dl->AddCircleFilled({pad.l + s.live.xy[i][0] / 1919.f * (pad.r - pad.l), pad.t + s.live.xy[i][1] / 941.f * (pad.b - pad.t)}, 9 * k, col(246, 190, 30));
        if (o.touch)
            for (int f = 0; f < (o.two ? 2 : 1); ++f) {
                const float fx = f ? 1 - o.x : o.x;
                dl->AddCircleFilled({pad.l + fx * (pad.r - pad.l), pad.t + o.y * (pad.b - pad.t)}, 11 * k, blue);
            }
    } else {
        char value[32];
        std::snprintf(value, sizeof(value), "Speed %d%%", int(o.sensitivity * 100 + .5f));
        centered(screen_rect(s, {254, 134, 368, 164}), value, fg, fs);
    }
    for (int b = 0; b < B_COUNT; ++b) {
        if (!visible(s, b)) continue;
        const Rect r = screen_rect(s, base_rect(b));
        const bool tab_on = (b == B_TAB_TOUCH && s.tab == 0) || (b == B_TAB_SPRAY && s.tab == 1);
        const bool active = b == s.pressed || (b == B_SPRAY && s.mode == 2) || (b == B_MOUSE && s.mode == 1) ||
                            (b == B_LEFT && o.left) || (b == B_RIGHT && o.right) || (b == B_BOTH && o.both) ||
                            (b == B_CLICK && o.centre) || (b == B_OPTIONS && o.options);
        ImU32 c = b == B_CLOSE ? (active ? col(196, 43, 28) : col(14, 15, 18)) : active ? blue : col(42, 46, 54);
        if (b <= B_TAB_SPRAY) c = tab_on ? col(52, 58, 70) : col(30, 33, 40);
        dl->AddRectFilled({r.l, r.t}, {r.r, r.b}, c, b == B_CLOSE ? 0 : 8 * k);
        if (b <= B_TAB_SPRAY && tab_on) dl->AddRectFilled({r.l + 8 * k, r.b - 3 * k}, {r.r - 8 * k, r.b}, blue);
        const char* label = b == B_SPRAY && s.mode == 2 ? "SPRAY MODE ON   .   F7 TO STOP"
                            : b == B_MOUSE && s.mode == 1 ? "Mouse touchpad: ON  (F6)" : labels[b];
        centered(r, label, b <= B_TAB_SPRAY && !tab_on ? dim : fg, fs);
    }
    DrawLive(dl, font, s, small, alpha);
    const Rect g = s.tab == 0 ? screen_rect(s, {14, 394, W - 14, LIVE_Y - 6}) : screen_rect(s, {14, 178, W - 14, LIVE_Y - 6});
    dl->AddText(font, small, {g.l, g.t}, dim, s.tab == 0 ? touch_guide : spray_guide, nullptr, g.r - g.l);
}
}

extern "C" __declspec(dllexport) void iss_gpu_pad_panel_toggle(void) {
    auto& s = iss::pad_ui::S();
    std::lock_guard lock{s.mutex};
    s.shown = !s.shown;
    if (!s.shown) iss::pad_ui::release_all(s);
}
extern "C" __declspec(dllexport) void iss_gpu_pad_panel_read(PadPanel* out) {
    auto& s = iss::pad_ui::S();
    std::lock_guard lock{s.mutex};
    *out = s.out;
    s.out.swipe = s.out.mode = -1;
    s.out.recentre = 0;
}
extern "C" __declspec(dllexport) void iss_gpu_pad_panel_mode(int mode) {
    auto& s = iss::pad_ui::S();
    std::lock_guard lock{s.mutex};
    s.mode = mode;
    if (mode == 2) { s.tab = 1; iss::pad_ui::release_all(s); }
}
// 1 while the pointer is on the visible panel: the mouse modes of src/pad.c ignore those clicks.
extern "C" __declspec(dllexport) int iss_gpu_pad_panel_covers(void) {
    auto& s = iss::pad_ui::S();
    std::lock_guard lock{s.mutex};
    return s.shown && s.mode != 2 && s.over ? 1 : 0;
}
extern "C" __declspec(dllexport) void iss_gpu_pad_panel_report(const PadLive* live) {
    auto& s = iss::pad_ui::S();
    std::lock_guard lock{s.mutex};
    s.live = *live;
}
