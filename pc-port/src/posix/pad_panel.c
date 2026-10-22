// SPDX-License-Identifier: GPL-2.0-or-later
// Linux: the touchpad panel (F5 in game) is drawn by libiss-gpu.so over the game window
// (gpu/pad_panel_ui.cpp), bound here once the library is loaded. The mouse modes of src/pad.c
// (F6 touchpad, F7 spray can) work; the spray can reads SDL's relative mouse.
#include "../pad_panel.h"
#include <SDL3/SDL.h>
#include <string.h>

static void (*panel_toggle)(void);
static void (*panel_read)(PadPanel *);
static void (*panel_mode)(int);
static int (*panel_covers)(void);
static void (*panel_report)(const PadLive *);
static int shown_mode;

void pad_panel_bind(void *toggle, void *read, void *mode, void *covers, void *report) {
    panel_report = (void (*)(const PadLive *))report;
    panel_toggle = (void (*)(void))toggle;
    panel_read = (void (*)(PadPanel *))read;
    panel_mode = (void (*)(int))mode;
    panel_covers = (int (*)(void))covers;
}
void pad_panel_report(const PadLive *live) { if (panel_report) panel_report(live); }
void pad_panel_toggle(struct SDL_Window *game) { (void)game; if (panel_toggle) panel_toggle(); }
// A stand-in handle while the pointer is on the panel, so src/pad.c leaves those clicks to it.
int pad_panel_covers(void) { return panel_covers && panel_covers(); }
void pad_panel_set_mode(int mode) {
    shown_mode = mode;
    if (panel_mode) panel_mode(mode);
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window) SDL_SetWindowRelativeMouseMode(window, mode == 2);
}
void pad_panel_read(PadPanel *out) {
    memset(out, 0, sizeof(*out));
    out->swipe = out->mode = -1;
    out->sensitivity = 1;
    if (panel_read) panel_read(out);
}
int pad_panel_raw(float *dx, float *dy) {
    *dx = *dy = 0;
    if (shown_mode != 2) return 0;
    SDL_GetRelativeMouseState(dx, dy);
    return 1;
}
