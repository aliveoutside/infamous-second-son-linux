// SPDX-License-Identifier: GPL-2.0-or-later
// The touchpad window (F5 in game, src/pad_panel.c), read by src/pad.c.
#pragma once
#include <stdint.h>
struct SDL_Window;
typedef struct {
    int touch,two,click;float x,y; // a finger on the window's pad (0..1); Shift: two; right button: click
    int left,right,both,centre,options; // buttons held down (options: the pause button)
    int swipe,mode,recentre;       // once: swipe 0 up 1 down 2 left 3 right (-1 none), mode 1/2 (-1 none)
    float sensitivity;             // the spray can's speed (1 = 100%)
    int hints;                     // once: the on-screen help was set to 0 auto, 1 always, 2 off (-1 none)
} PadPanel;
// The physical controller as it is now, for the panel to show (src/pad.c fills it every read).
typedef struct {
    int connected;
    uint32_t sources;             // bit i: button i of the pad pressed (A B X Y LB RB LT RT L3 R3 View Menu Guide Share Up Down Left Right)
    uint8_t lx,ly,rx,ry,lt,rt;    // sticks (128 = centre) and triggers, as the controller reports them
    int contacts[2];uint16_t xy[2][2]; // the touchpad fingers the game gets (1920 x 942), from any source
    int click;                         // the touchpad is pressed
} PadLive;
void pad_panel_report(const PadLive *live);
void pad_panel_toggle(struct SDL_Window *game); // shows or hides the panel over the game
void pad_panel_read(PadPanel *out); // the window's input since the last read
void pad_panel_set_mode(int mode);  // the mouse mode pad.c is in (0 off, 1 touchpad, 2 spray)
int pad_panel_raw(float *dx,float *dy); // the mouse's raw counts since the last call; 0 while Raw Input is not delivering
int pad_panel_covers(void); // the panel is under the mouse
void pad_panel_bind(void *toggle,void *read,void *mode,void *covers,void *report); // the GPU library's panel (gpu/pad_panel_ui.cpp)
