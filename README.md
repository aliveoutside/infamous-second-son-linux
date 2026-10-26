# inFAMOUS Second Son: native Linux port

![Delsin above Seattle at night, neon power](docs/media/game.jpg)

An experimental native Linux port of the PS4 game *inFAMOUS Second Son*. The game's own x86-64
code runs directly on your CPU. Its rendering goes to Vulkan through a renderer built on
[shadPS4](https://github.com/shadps4-emu/shadPS4)'s video core.

> **You need your own copy of the game** (CUSA00004 or CUSA00223, version 1.07). This repository contains no
> game files, keys or copyrighted assets.

[▶ Video showcase](https://youtu.be/V8LJlxHLb_Q)

## Features

- The game's own x86-64 code runs directly on your CPU. The system libraries and the GPU side are replaced.
- Vulkan renderer based on shadPS4, with a shader cache.
- Mouse look and aim like in a PC shooter. Keyboard, Xbox and PlayStation controllers and the
  Steam Deck work too.
- FSR 3 anti-aliasing and frame generation.
- Resolution, FOV, vsync and frame limit settings, ultrawide support.
- Performance mode for slower machines.
- MangoHud support and a built-in FPS overlay (F9).
- All game languages.
- AppImage with a launcher. Pick your game folder and press PLAY.

## Build and run

You need [just](https://github.com/casey/just), podman or docker, python3 and a Vulkan driver.
Everything builds in an Ubuntu 24.04 container, so the result runs on any distro with glibc 2.39
or newer.

```bash
git clone --recursive https://github.com/Castlesea/infamous-second-son-pc-port.git
cd infamous-second-son-pc-port
just setup "<your game folder>"
just play
```

`just build` also makes `pc-port/build/inFAMOUS_Second_Son-x86_64.AppImage`, which you can hand
out: it contains no game files. Players choose their own unpacked copy of the game in the
launcher, and the first PLAY prepares it into `~/.local/share/iss-pc/appimage/`.

Settings are in `~/.config/iss-pc/settings.toml`.

<img src="docs/media/launcher.png" alt="The launcher, controls tab" width="600">

## Controls

Xbox and PlayStation controllers, the Steam Deck and the keyboard all work. On controllers
without a touchpad, View and Menu press its left and right side. F5 opens a panel for touchpad
input and the spray can.

| Key | Action |
|---|---|
| W A S D, mouse | Move, camera and aim |
| Left / right / middle button | Fire (R2) / aim (L2) / R3 |
| Space / Shift / F / E | Jump / power dash / melee / subdue and heal |
| Q / R | L1 / R1: the current power's moves |
| Ctrl / M | L3 / R3 |
| Up / Left / Right arrows, G | D-pad (G is Down: ultimate) |
| Esc | Options |
| F4 | Mouse aim on / off (`mouse_aim` setting) |
| Z, X | Touchpad left, right |
| Hold Alt | Mouse as touchpad while held (left button finger, right click, wheel or side button R2) |
| F6 | The same, switched on and off |
| F5 / F9 / Alt+Enter | Touchpad panel / FPS overlay / fullscreen |

The mouse turns the camera directly, like in a PC shooter: no stick emulation, no acceleration.
Details in [pc-port/docs/mouse-and-keyboard.md](pc-port/docs/mouse-and-keyboard.md).

## Credits

Based on the original Windows port by [Castlesea](https://github.com/Castlesea). The renderer
builds on [shadPS4](https://github.com/shadps4-emu/shadPS4) and
[weshayoub92/shadPS4-perf](https://github.com/weshayoub92/shadPS4-perf).

## License

GPL-2.0-or-later. *inFAMOUS* is a trademark of Sony Interactive Entertainment. This project is
not affiliated with Sony or Sucker Punch Productions.
