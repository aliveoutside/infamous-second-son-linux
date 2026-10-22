# pc-port

The source of the port. Build and run instructions are in the [top-level README](../README.md).

| Path | What it is |
|---|---|
| `src/` | `iss-probe`: maps the game image, runs the game's x86-64 code and replaces the PS4 system libraries. `src/settings.c` reads `settings.toml` ([docs/settings.md](docs/settings.md)). |
| `gpu/` | `libiss-gpu.so`: shadPS4's video core with this port's overrides and native passes. |
| `native/` | Replacements for engine routines, checked against the 1.07 eboot before they are patched in. |
| `scripts/` | Game preparation (`prepare.py`, `link_modules.py`), `linux/setup.sh`, `linux/play.sh`, and `analyze_performance.py` for frame logs. |
| `tests/` | Unit tests: `ctest --test-dir build`. |
| `pgo/` | PGO profile ([docs/pgo.md](docs/pgo.md)). |
| `third_party/shadPS4` | The shadPS4 fork, a git submodule. |
| `out/` | Git-ignored: the prepared game image and caches. It contains game code, so never publish it. |

`THIRD_PARTY.md` lists where code came from and its
licence.

## Development

Internal options are environment variables (`ISS_*`, `BB_*`). Set one in the environment or under
`[developer]` in `settings.toml`, for example `ISS_GPU_PROFILE=1 scripts/linux/play.sh`. The
tested defaults are in `src/settings.c`.

`SHADPS4_FRAME_CSV=frames.csv` writes per-frame timings, and
`python3 scripts/analyze_performance.py frames.csv` summarises them.

## Game

| | |
|---|---|
| Game | CUSA00004 (Europe) or CUSA00223 (North America, same `eboot.bin`), version 01.07 |
| eboot SHA-256 | `2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab` |
