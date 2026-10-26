# Settings

Players set everything in `~/.config/iss-pc/settings.toml` (or `$XDG_CONFIG_HOME/iss-pc/`). The
first start writes it with every setting, its default and a comment. Delete it to get the
defaults back. On a Steam Deck the new file starts with `1280x800`, `fullscreen = "on"` and a
`pad_map` for the Deck ([steam-deck.md](steam-deck.md)).

| Setting | Default | Values |
|---|---|---|
| `game_dir` | | The game folder (with `eboot.bin`) |
| `save_dir` | `<data>/savedata` | Folder for saves |
| `resolution` | `1920x1080` | `WIDTHxHEIGHT`; wider than 16:9 is Hor+ |
| `fullscreen` | `off` | `on`, `off` (Alt+Enter in game) |
| `vsync` | `mailbox` | `on`, `off`, `mailbox` |
| `fps_limit` | `60` | `0` = unlimited |
| `fsr_aa` | `off` | `on`: FSR 3 replaces the game's SMAA T2x resolve at the render size. Switched in the F1 menu too, which saves it here |
| `fsr_sharpness` | `0` | FSR 3 sharpening, 0 to 100 percent. F1 menu too, live |
| `frame_generation` | `off` | `on`: FSR 3 frame generation, one frame shown between two real ones, paced half a frame apart (needs `fsr_aa`). Skipped after a hitch (over 2.5 times the usual frame, at least 50 ms) or a frame the game rendered twice. F1 menu too |
| `mangohud` | `off` | Launcher only: run the game under MangoHud, if installed |
| `fov` | `100` | 50 to 200, percent of the game's |
| `hud_width` | `16:9` | `16:9`, `21:9`, `full` |
| `language` | `english` | english, french, italian, german, spanish, dutch, portuguese, russian, polish, turkish; danish, finnish, norwegian, swedish (subtitles only); latino, brazilian (CUSA00223 only) |
| `gpu_memory` | `video` | `video`, `in_place` (much slower on NVIDIA) |
| `shaders` | `wait` | `wait`, `parallel` (more compiler threads), `skip` (draws of compiling pipelines skipped) |
| `shader_precompile` | `off` | `on`: compute shaders compiled ahead of time in the background, `off` |
| `native_passes` | `on` | The engine threads record whole render passes into Vulkan, so the command processor thread, the limit in busy city views, translates far fewer draws. Needs a restart. Set `off` if the driver crashes (NVIDIA 595 under Linux did in iss_pc's tests) |
| `performance_mode` | `off` | The game's own lower quality settings (eboot 1.07 only, bytes checked first): `low` (LOD 0.7, one sun-shadow cascade less, half-resolution particles, 2 high-res texture blocks), `office` (LOD 0.5, quarter-resolution particles, no high-res blocks, half the pedestrians). Measured by axylpup: +13% / +19% in the heaviest city view |
| `mouse_aim` | `on` | Mouse look and aim from the start (left button fires, right aims); F4 toggles in game |
| `mouse_sensitivity` | `100` | Mouse look speed, 10 to 1000 percent (100: 0.046° per mouse count, see [mouse-and-keyboard.md](mouse-and-keyboard.md)) |
| `hints` | `auto` | On-screen help for the touchpad (hold Alt) and the spray can: `auto` (in full the first 3 times per mode, then a small label; count in `<data>/hints-seen`), `always`, `off`. Also switched in the F5 panel, which saves it here |
| `deadzone_left` | `10` | Left stick dead zone, 0 to 50 percent |
| `deadzone_right` | `10` | Right stick dead zone, 0 to 50 percent |
| `pad_map` | | Controller layout for pads without a touchpad. Sources include the Deck's back buttons `l4` `l5` `r4` `r5` |

## Launcher

`build/iss-launcher` edits the same file in a window (Display, Graphics, Game, Controls,
Folders; mouse, keyboard or controller) and starts the game with PLAY, which saves first and runs
`scripts/linux/play.sh`. The window hides while the game runs and comes back when it exits. The
game's output goes to `<data>/logs/game.log` (the run before: `game.prev.log`). Comments and the
`[developer]` section of `settings.toml` are kept when it saves.
`scripts/linux/install-shortcut.sh` adds it to the application menu with the game's icon.

Bad values are reported at start and replaced by the default. The data folder (caches, saves)
is `build/user` when it exists, else `~/.local/share/iss-pc`.

## For developers

Every option is a key of this file and nothing else: there are no environment variables behind
them. `src/settings.c` holds the table (key, default, check) and reads the file at start; the code
asks for a key with `settings_get("fsr_aa")` (`src/settings.h`), which gives the code value of a
choice (`on` is `"1"`) or NULL when empty. `libiss-gpu.so` reaches the same function with
`iss::Setting` (`gpu/settings_value.h`, `iss-probe` exports it), even while it loads. A change made
in game (F1 menu, F5 panel) goes through `settings_set` and is saved to the file. The launcher
reads the same table. A new key needs its row above and a case in
`tests/settings_test.c`.

The renderer's tuning constants are compiled in rather than exposed: the values that were measured
once (barrier pooling, the write fault windows, the staging keeps, the pipeline and draw paths,
the VRAM budget) are what plays. There is no `[developer]` section any more; an old
`settings.toml` that still has one loads (its keys are ignored) and loses it at the next save.

Three environment variables are left, each for one run rather than for a player:
`ISS_SETTINGS=<file>` (another settings file), `ISS_USER_DIR=<folder>` (another data folder) and
`SHADPS4_FRAME_CSV=frames.csv` (per-frame timings). The program also sets
`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536` itself and restarts once, so no wrapper
script is needed.
