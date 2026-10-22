# Mouse and keyboard

How keyboard and mouse play works, and what is known about the game's input and camera code.
Offsets are image offsets of the 1.07 eboot (sha256 `2d1ca796...`); at run time the image starts at
`0x10000000`, so eboot `+0x14e95a` is host address `0x1014e95a`.

## What the player gets

- The mouse looks and aims from the start (`mouse_aim = on`, env `ISS_MOUSE_AIM`; F4 toggles). SDL
  relative mouse mode. Left button = R2 (fire), right = L2 (aim), middle = R3.
- The mouse turns the camera by an **angle**: the same hand distance gives the same turn at any speed,
  no dead zone, no acceleration, no delay. `mouse_sensitivity` (percent, 10 to 1000, env
  `ISS_MOUSE_SENS`): 100% = 0.0008 rad (0.046°) per mouse count, 7854 counts per full turn.
  Tested in game by the user: it feels native.
- Keyboard (table `keys[]` in `pad_read`, `src/pad.c`): W A S D move, Space = Cross (jump),
  Left Shift = Circle (power dash, menu back), F = Square (melee), E = Triangle (subdue / heal),
  Q / R = L1 / R1 (the current power's moves), Left Ctrl = L3, M = R3, Esc = Options,
  Up, Left, Right arrows = D-pad, G = D-pad Down (ultimate; the Down arrow is free), U / O = L2 / R2 when mouse aim is off, I J K L = right stick.
  Touchpad: Z / X (left / right side, right side = absorb), T centre, 1-4 swipes (`touch_keys`).
  F4 mouse aim, F5 touchpad panel, F6 mouse as touchpad, or hold Left Alt for the same until it is let go (`touchpad_hold` in `ISS_TOUCH_KEYS`; an on-screen hint names the key, and once, on the first run, tells where to find it; marker file `touchpad-hint-seen` in the user folder) (left button = finger, right = touchpad click,
  middle or a side button = R2 held, so a touchpad move that needs R2 is one hand; O also holds R2), F7 spray can, F9 FPS overlay, F10 perf mark.
  Graffiti: while a stencil is on screen (it reads `GYRO_Y_AXIS`, `GYRO_RETICLE_POS` and tests the SprayShake button, hooked by `native/graffiti.cpp`;
  `ISS_GRAFFITI_AUTO=0` turns it off) the mouse is the spray can by itself, as F7: move = aim, left
  button = spray (R2), right = shake, middle = recentre, Shift = fine. A controller without a gyro
  aims with the right stick and shakes with L2; a DualShock 4 / DualSense uses its own gyro. The can
  is kept as yaw and pitch within the game's aim window (`pad_spray_step`): the game's aim
  (`+0x24b790`) is asin of the light bar's direction, +-0.35 rad of yaw by +-0.25 rad of pitch.
  Button meanings come from StrategyWiki (InFAMOUS Second Son/Controls); what L3, R3 and D-pad
  left / right / up do is not documented anywhere found.

## Why the mouse cannot simply be the right stick

The stick sets the camera's turning **speed**. Any mouse-to-stick conversion (this port's first
attempts, shadPS4's `input_mouse.cpp`) behaves like a stick: a fast flick is capped by the maximum
turn rate or paid out late, and the game adds its own dead zone, response curve and acceleration
(80 to 160 deg/s ramps). So the camera code is patched instead (below). The stick conversion is kept
as the fallback for an eboot that does not match (`aim_step` in `src/pad.c`: distance kept as a debt
and paid out as stick deflection, K = 2400 px/s at 100%, `pad_info` then reports `right_dead = 0`).

## The patch (`native/mouse_look.cpp`)

1. eboot `+0x14ddd0` updates the player camera (`rdi` = camera object). It reads the right stick
   through the pad's virtual `GetStick` (vtable `+0x70`, eboot `+0x24d740`, call at `+0x14e5cb`),
   applies the invert options (bytes eboot `+0xefb3cd` x, `+0xefb3ce` y), shapes the stick into turn
   rates and calls the integrator eboot `+0x14d6f0` at eboot `+0x14e95a`:
   `Integrate(camera, settings (rsi), mode settings (rdx), rcx, float *angles (r8), float *rates (r9), x (xmm0), y (xmm1))`.
   It writes the new yaw and pitch (radians) to `angles[0..1]` (trapezoid: old + (old rate + new
   rate) * dt / 2, yaw wrapped to ±π) and the rates to `rates`. The caller stores them at
   camera `+0x330` (angles) and `+0x340` (rates).
2. The `call` at `+0x14e95a` is redirected to a `movabs rax; jmp rax` written in the 15 NOP bytes at
   eboot `+0x140641`, which jumps to `Integrate` in `mouse_look.cpp`: the original runs, then the mouse
   counts gathered since the last update are added: `yaw -= dx * scale` (stick or mouse to the right
   lowers the yaw), `pitch += dy * scale`, honouring the game's invert options.
3. Pitch limits: `[max(rsi[0x1c0], rdx[0x1c0]), min(rsi[0x1c4], rdx[0x1c4])]`, on foot -1.45 to 1.525.
   The mouse never pushes pitch further past a limit than the game had it. `rdx + 0x210` is a narrow
   range (0.05..0.06 on foot): the default pitch the camera returns to when the stick is idle.
4. Camera `+0x470` (int): right stick state, 0 idle / 1 moving / 2 full, with the times of the
   changes at `+0x474`, `+0x478`, `+0x47c` (state machine eboot `+0x149050`, clock int at eboot
   `+0xe162a4`). The patch sets it to 1 while the mouse moves, so the return to the default pitch
   waits for the mouse to stop as it does for the stick.
5. Identity checks (FNV-1a 64, `CodeHash` from `ultrawide.cpp`): the integrator (`+0x14d6f0`, 0x5a0
   bytes), the call site's argument setup (`+0x14e936`, 0x29 bytes) and the padding. On a mismatch the
   patch is skipped and the mouse falls back to the stick.
6. Auto-follow: the camera swings back behind the player by itself. The right stick turns that off through
   the ManualCamera event, the mouse does not. The only reader of the game's "no auto-follow" byte
   (eboot `+0xe16c40`) is `+0x14936e`, in `+0x149360`, which then takes the game's own off path. The hook
   sets the byte while the mouse moves and clears it `ISS_MOUSE_FOLLOW_HOLD` seconds (default 5, 0 = leave
   auto-follow alone) after the last move. Found in axylpup/shadPS4 (`src/core/mouse_aim.cpp`).
7. `src/pad.c` gathers the counts in `look[]` under the pad lock; `pad_look_take` hands them to the
   camera and clears them. If the camera has not taken them for two pad reads (menus, cutscenes), they
   are dropped, so the view does not jump when play resumes. `pad_look_direct` is set by the installer.

## The pad object (reverse engineering notes)

- Two pad objects are globals: pad 0 at eboot `+0x1d1c3f0`, pad 1 at `+0x950` from it. Vtable at
  eboot `+0xcddf70` (pointers are only present after relocations: apply the `boot.bin` relocations of
  kind 0 before searching data for function pointers).
- `+0x24a280` updates both pads each frame through `+0x24a2c0`: `+0x24b010` reads the pad
  (`scePadReadState` thunk `+0x984d80`, called only from `+0x24b17d`, raw sticks as bytes at pad
  `+0xd4..0xd7`), `+0x24b340` buttons (`pad+0x20`) and the stick filter `+0x249170`, then `+0x24b790`
  (motion), `+0x24c320` (light bar), `+0x124830` (button combo listeners).
- Stick filter `+0x249170`: objects at pad `+0x1a0` (left) and `+0x420` (right). It keeps the raw bytes
  at object `+0`, a 30-entry history ring (16 bytes each, from object `+0x34`: time, x, y, magnitude),
  running maxima, a radial dead zone and curve (`+0x618c40`), snap-back detection (the last direction
  is held when the magnitude falls faster than 10/s) and flick detection (`+0x248030`). Outputs: left
  x, y, magnitude at pad `+0x08/0x0c/0x10`, right at `+0x14/0x18/0x1c`.
- **Nobody reads pad `+0x14/+0x18`** (checked with hardware watchpoints in gameplay). The camera calls
  `GetStick` (vtable `+0x70`), which recomputes the stick from the raw bytes in the stick object.
  Other getters: vtable `+0x78` flick state (`+0x24c5a0`), `+0x80` last direction, `+0x90` history
  min/max over a time window.
- Pad `+0x7f0` is a control flag checked by camera helpers (`+0x12d3a0`, `+0x15e1f0`); eboot `+0x12d3d0`
  is a stick acceleration helper (80/160, 75/85 deg/s constants) that nothing calls.

## Debugging tips

- Run with `ISS_PAD_SCRIPT=turn ISS_PAD_SCRIPT_DELAY=40 scripts/linux/play.sh`: after 40 s of process
  time the right stick is held at 168 (camera turns slowly right); the save loads by itself and the
  city is reached after about 80 to 120 s.
- gdb attaches (`ptrace_scope` 0). The runtime uses SIGSEGV on purpose: `handle SIGSEGV nostop noprint
  pass` (and SIGUSR1/2). The guest code cannot take software breakpoints ("Cannot access memory"):
  use `hbreak`. Hardware `rwatch` also fires on writes on x86; filter by RIP. The process is
  `build/iss-probe` (comm `exe`): `pgrep -f build/iss-probe`; do not `pkill -f iss-probe` from a shell
  whose command line contains that text.
- `iss-probe` keeps its symbols: in gdb `look`, `mouse_mode`, `pad_look_direct` can be read and set.
- Static analysis: extract the flat image from `out/boot.bin` (48-byte header `ISSPRB01`, size, entry,
  counts; 24 bytes per segment, 128 per import name, 32 per relocation (target, kind, value, addend),
  then the image). IDA: load the code part as a 64-bit binary at base 0, create functions by hand
  (`ida_funcs.add_func`), then decompile. `objdump -D -b binary -mi386:x86-64 -M intel` also works.
