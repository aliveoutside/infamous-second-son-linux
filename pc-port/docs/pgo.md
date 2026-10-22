# Profile-guided optimization (PGO)

`libiss-gpu.so` is built with a GCC profile recorded while playing: the compiler lays out and
inlines the hot paths (the command processor thread) from real counts. With LTO it makes the
command processor ~27% cheaper per draw.

The profile lives in `pgo/` and is part of the repository, like Chromium and Firefox ship
theirs. Training needs the game, which is not in the repository, so it cannot run in CI.

## Using it

Nothing to do: `just build` finds `pgo/*.gcda` and builds with it (`-- libiss-gpu PGO: use`). It is skipped, and the build is a normal one, when:

- the compiler is not GCC, or its major version differs from `pgo/gcc-version`
  (GCC profiles are not portable between major versions);
- `-DISS_PGO=off` is passed.

Functions changed since the training compile without profile data (`-fprofile-partial-training`),
so a slightly stale profile is still fine. The container's GCC (14) is the one the profile is trained
with.

The file names are relative to the build folder (`-fprofile-prefix-path`), so the profile works
from any checkout location and any build folder name.

## Retraining

Do it after larger renderer changes or a change of the container's GCC.

```bash
just pgo
```

It builds an instrumented library in `build-pgo/` (in the container, like every build) and starts
the game. Play the slow scenes for a few minutes (the burning house at the start of the story
save works well); the library writes the profile every 20 s, because the game ends without exit
handlers. Then:

```bash
just build
git add pgo/ && git commit -m "Retrain PGO profile"
```

`ISS_PGO=generate` also writes `pgo/gcc-version`.
