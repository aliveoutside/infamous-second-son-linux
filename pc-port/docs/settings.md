# Settings

Players set everything in `~/.config/iss-pc/settings.toml` (or `$XDG_CONFIG_HOME/iss-pc/`). The
first start writes it with every setting, its default and a comment. Delete it to get the
defaults back.

| Setting | Default | Values |
|---|---|---|
| `game_dir` | | The game folder (with `eboot.bin`) |
| `save_dir` | `<data>/savedata` | Folder for saves |
| `resolution` | `1920x1080` | `WIDTHxHEIGHT`; wider than 16:9 is Hor+ |
| `fullscreen` | `off` | `on`, `off` (Alt+Enter in game) |
| `vsync` | `mailbox` | `on`, `off`, `mailbox` |
| `fps_limit` | `60` | `0` = unlimited |
| `fsr` | `off` | FSR 1 upscaling: `on`, `off` |
| `mangohud` | `off` | Launcher only: run the game under MangoHud, if installed |
| `fov` | `100` | 50 to 200, percent of the game's |
| `hud_width` | `16:9` | `16:9`, `21:9`, `full` |
| `language` | `english` | english, french, italian, german, spanish, dutch, portuguese, russian, polish, turkish; danish, finnish, norwegian, swedish (subtitles only); latino, brazilian (CUSA00223 only) |
| `gpu_memory` | `video` | `video`, `in_place` (much slower on NVIDIA) |
| `shaders` | `wait` | `wait`, `parallel` (more compiler threads), `skip` (draws of compiling pipelines skipped) |
| `shader_precompile` | `off` | `on`: compute shaders compiled ahead of time in the background, `off` |
| `mouse_aim` | `on` | Mouse look and aim from the start (left button fires, right aims); F4 toggles in game |
| `mouse_sensitivity` | `100` | Mouse look speed, 10 to 1000 percent (100: 0.046° per mouse count, see [mouse-and-keyboard.md](mouse-and-keyboard.md)) |
| `hints` | `auto` | On-screen help for the touchpad (hold Alt) and the spray can: `auto` (in full the first 3 times per mode, then a small label; count in `<data>/hints-seen`), `always`, `off`. Also switched in the F5 panel, which saves it here |
| `deadzone_left` | `10` | Left stick dead zone, 0 to 50 percent |
| `deadzone_right` | `10` | Right stick dead zone, 0 to 50 percent |
| `pad_map` | | Controller layout for pads without a touchpad |

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

The runtime and the renderer still read their internal options from environment variables
(`ISS_*`, `BB_*`, `SHADPS4_*`). Players never need them: `src/settings.c` fills them in at start,
in this order:

1. a variable already set in the environment (`ISS_GPU_PROFILE=1 scripts/linux/play.sh`);
2. `settings.toml`, including raw variables under `[developer]`;
3. the tested defaults in `src/settings.c` (`tuned_defaults`).

`ISS_SETTINGS=<file>` uses another settings file. The program also sets
`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=65536` itself and restarts once, so no wrapper
script is needed.
