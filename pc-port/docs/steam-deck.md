# Steam Deck

Untested on a real Deck so far: written from SDL3's and Steam's documented behavior. Please report
what the log (`~/.local/share/iss-pc/logs/game.log`, the `PAD selected` line) shows.

## What is set for the Deck

On a Deck (board `Jupiter` or `Galileo`, read from the firmware) the first start writes
`settings.toml` with:

- `resolution = "1280x800"` (the screen's 16:10; the picture is Hor+/Vert-fitted by the ultrawide
  patch, nothing is cropped) and `fullscreen = "on"`. An existing file is not changed.
- `pad_map = "menu=options,l4=touch_right,l5=touch,r4=l3,r5=r3"`: the Menu button pauses (Options),
  View presses the touchpad's left side, the back buttons press its right side and centre and
  click the sticks. The Deck has no touchpad of its own the game can read, so use these buttons (or
  hold L3 + R3 for the stick-as-finger layer, see the launcher's Controls tab).

## Controller and gyro (graffiti)

The port turns on SDL's Steam Deck driver, which reads the Deck's controller directly: gyro (spray
can minigame) and the four back buttons. Steam must not own the controller for that:

1. Add the game to Steam (below).
2. In its Controller settings choose **Disable Steam Input**.

With Steam Input on, the game gets Steam's virtual Xbox pad: everything works except the gyro and the
back buttons (graffiti then uses the sticks, as without a gyro on a PC).

## Adding the game to Steam (Game Mode)

In Desktop Mode run `scripts/linux/install-shortcut.sh` (the AppImage: just start it once), then in Steam
**Add a Non-Steam Game** and pick *inFAMOUS Second Son*, which starts the launcher; PLAY starts the game
(the launcher is controller-navigable). Set **Properties > Compatibility** to nothing: this is a
native Linux program. The Steam button, the Quick Access menu and the keyboard overlay work as for any
non-Steam game.

## Performance

- The default renderer settings are tuned on a desktop GPU; start with `fps_limit = "30"` if 60 does
  not hold, and keep `fsr_aa` and `frame_generation` off (they cost GPU time).
- `performance_mode = "low"` lowers the game's own LOD, shadow and particle quality.
- The first start compiles shaders (a few minutes); the caches are in `~/.local/share/iss-pc`.
