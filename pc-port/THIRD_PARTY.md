# Provenance

`libiss-gpu.so` (`gpu/CMakeLists.txt`) compiles the Vulkan renderer, shader recompiler,
GnmDriver, VideoOut, AJM and Videodec sources of `third_party/shadPS4`, with the shims in
`gpu/` and without the emulator's main program and loader. That integration stays under
GPL-2.0-or-later. Some shims were adapted from the Bloodborne reference cited below. Third-party
libraries (including SDL, Vulkan, ImGui, FFmpeg and fmt) stay in their existing trees with their
own notices and licences; they are not relicensed.

This project is GPL-2.0-or-later; the full text is in `LICENSE`.

`src/pad.c` uses the SDL3 API and the quaternion integration convention of
`shadPS4/src/input/controller.cpp`. `src/save_dialog.c` and `gpu/save_dialog_ui.cpp` adapt the
layouts and states of `shadPS4/src/core/libraries/save_data/dialog/`.

The launcher (`launcher/launcher.cpp`) uses the Oswald font (`launcher/assets/fonts/Oswald.ttf`,
SIL Open Font License 1.1, `launcher/assets/fonts/OFL.txt`), Dear ImGui and stb_image (MIT /
public domain, from the shadPS4 checkout's `externals`).

`scripts/ps4_format.py` adapts the SELF/SFO/NID reading functions of
[`deadinside28/bloodborne_pc/scripts/prepare.py`](https://github.com/deadinside28/bloodborne_pc/blob/4d4d20812c0af23a11c8b8d40f5e06a9054925ef/scripts/prepare.py),
snapshot `4d4d20812c0af23a11c8b8d40f5e06a9054925ef`, GPL-2.0-or-later. The segment limits and
checks were kept; the `game_check` import and the rest of the Bloodborne-specific preparer were
removed.

The NID/library/module identity format was checked against
[`scripts/link_libc.py`](https://github.com/deadinside28/bloodborne_pc/blob/4d4d20812c0af23a11c8b8d40f5e06a9054925ef/scripts/link_libc.py)
and `third_party/shadPS4/src/core/module.cpp` and `loader/elf.h`. The `ISSPRB01`/`ISSPRB02`
image formats and this project's loader are its own. `scripts/link_modules.py` follows the cited
project's organisation but resolves exact identities and versions.

The runtime links Zydis/Zycore (MIT) from the shadPS4 checkout (`externals/zydis`). The TCB/DTV layout was checked against
`src/core/tls.h`/`linker.cpp` of shadPS4 and `bloodborne_pc/src/runtime_thread.c`. The contract
and layout of `sceLibcHeapGetTraceInfo` were adapted from
`src/core/libraries/libc_internal/libc_internal_memory.cpp`, Copyright 2024 shadPS4 Emulator
Project, GPL-2.0-or-later. The shadPS4 base is the `third_party/shadPS4` submodule (upstream
`584374867d6a655d911a26fe06145f3a7cb2e89d` plus this port's changes on branch `iss-port`).

The import inventory reads shadPS4's local AeroLib table only to give indicative names to NIDs.
The table is not copied into this project, and names alone neither implement nor validate an API.

There are no game executables or assets in this project. `out/` is generated locally from the
user's own game files, contains game code and data, and is ignored by Git. Never publish `out/`.
