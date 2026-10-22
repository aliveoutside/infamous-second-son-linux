# AGENTS.md

Notes for AI agents working in this repository. Read the [README](README.md) for the user-facing
description and [pc-port/README.md](pc-port/README.md) for the source layout.

## What this is

A native Linux port of the PS4 game *inFAMOUS Second Son* (CUSA00004, v1.07). The game's own x86-64
code runs directly on the CPU; the port replaces the PS4 system libraries and sends rendering to
Vulkan through a renderer built on shadPS4's video core. The repository contains no game files,
keys or assets, and must never contain any.

## Layout

| Path | What |
|---|---|
| `pc-port/src/` | `iss-probe`: maps the game image, runs its code, replaces the system libraries (C). Input is `pad.c`, settings are `settings.c`. |
| `pc-port/gpu/` | `libiss-gpu.so`: shadPS4's video core plus this port's overrides (`gpu/overrides/`) and native passes (C++). Loaded by `iss-probe` with `dlopen`. |
| `pc-port/native/` | Replacements for engine routines, checked against the 1.07 eboot before patching. |
| `pc-port/launcher/` | `iss-launcher`: settings form and PLAY button (SDL3 + Dear ImGui). |
| `pc-port/scripts/` | Game preparation (`prepare.py`, `link_modules.py`, `content_profile.py`) and `linux/` build and run scripts. |
| `pc-port/tests/` | Unit tests, run with ctest. |
| `pc-port/pgo/` | PGO profile (`*.gcda`), see `pc-port/docs/pgo.md`. |
| `pc-port/docs/settings.md` | Every setting of `settings.toml`. |
| `pc-port/docs/mouse-and-keyboard.md` | Keyboard layout, mouse look (camera patch `native/mouse_look.cpp`), the game's pad and camera internals, gdb/IDA tips. |
| `pc-port/third_party/shadPS4` | shadPS4 fork, a git submodule. |

## Build

Everything builds in an Ubuntu 24.04 container so the result runs on any distro with glibc 2.39+.
Needs `just`, podman or docker, python3 and a Vulkan driver. Clone with `--recursive`.

```bash
just build                  # container build into pc-port/build, AppImage included
just test                   # build, then ctest in the container
just setup "<game folder>"  # prepare a game folder (the one with eboot.bin), once per folder
just play                   # build and start the game
just launcher               # build and start the launcher
```

`just` runs from `pc-port/`. The recipes call `scripts/linux/container.sh` with
`scripts/linux/container-build.sh`, which builds SDL3 once (from the shadPS4 submodule) into
`build/sdl3`, then configures and builds the port with Ninja. A single test:
`scripts/linux/container.sh sh -c 'cd build && ctest -R pad --output-on-failure'`.

CI is `.github/workflows/appimage.yml`: the same container build, the tests, the AppImage as an
artifact. Releases are uploaded by hand from a local build (`just build`, with PGO).

## Running and settings

- `scripts/linux/play.sh` starts `build/iss-probe` on the prepared image in `pc-port/out/`.
- Settings: `~/.config/iss-pc/settings.toml` (written on first start). Unknown keys are an error.
  Defaults and validation live in `pc-port/src/settings.c`; document new keys in `docs/settings.md`
  and add them to `tests/settings_test.c`. The launcher reads the same table.
- Internal options are `ISS_*` / `BB_*` environment variables, or `[developer]` in `settings.toml`.
- Game data, caches and logs of the AppImage: `~/.local/share/iss-pc/` (log: `logs/game.log`).
  When run from the build folder, they are in `pc-port/build/user/`.
- `SHADPS4_FRAME_CSV=frames.csv` writes per-frame timings; summarise with
  `python3 scripts/analyze_performance.py frames.csv`.

## Rules and gotchas

- **Never wipe `pc-port/build`.** Saves, trophies and caches live in `pc-port/build/user`. Delete
  CMake files only, not the folder.
- `pc-port/out/` holds the prepared game image (game code). It is git-ignored: never commit or
  publish it. Same for `build/`, profiling output (`frames.csv`, `trace.jsonl`,
  `performance-summary.json`).
- **One SDL3 instance.** `libiss-gpu.so` must link the shared SDL3 the host program uses (see
  `gpu/CMakeLists.txt`, `scripts/linux/shadps4_flags.py`, which drops shadPS4's static `libSDL3.a`).
  A second copy made the window invisible to the input code (no keyboard, no F5 panel). Check with
  `nm -D --defined-only build/gpu/libiss-gpu.so | grep -c ' SDL_'`, it must print 0.
- `play.sh` copies `build/gpu/libiss-gpu.so` over `build/libiss-gpu.so` when it is newer; copy it by
  hand if you run `iss-probe` directly.
- Input code in `pad.c` only reads the keyboard while a game window has focus.
- The mouse turns the camera through an eboot patch, not the right stick; read
  `docs/mouse-and-keyboard.md` before changing mouse or camera input.
- Match the surrounding style. `src/pad.c` is dense C; keep its idiom when editing it.

## Performance debugging

Profile with the frame CSV (`SHADPS4_FRAME_CSV`) and `ISS_GPU_PROFILE=1`. Before blaming the code
for a slowdown, rule out the system (CPU scheduler, GPU clocks) and shader compilation
(`AOT_COMPUTE ... compiled=N` in `game.log`; `compiled=0` means everything came from the cache).

## Tests

`pc-port/tests/*_test.c` and `test_*.cpp` are plain assert-style programs registered in
`pc-port/CMakeLists.txt`. Add new ones there. They do not need the game.
