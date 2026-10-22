#!/usr/bin/env bash
# Launches inFAMOUS Second Son from the build folder (ISS_BUILD, default build). Build first:
#   just build
# Settings: ~/.config/iss-pc/settings.toml (written on the first start; see docs/settings.md).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
b=${ISS_BUILD:-build}  # build-pgo while training PGO
# Until game_dir is set in settings.toml, the folder prepare.py recorded is used.
if [[ -z "${ISS_GAME_DIR:-}" && -f out/game_dir.txt ]]; then export ISS_GAME_DIR=$(head -n1 out/game_dir.txt); fi
[[ -x $b/iss-probe ]] || { echo "$b/iss-probe is missing: build the port first" >&2; exit 1; }
# The prepared image (setup.sh, or here on the first start, as in the AppImage).
if [[ ! -f out/boot-linked.bin || ! -f out/content_profile.txt ]]; then
  mkdir -p out
  python3 scripts/prepare.py
  python3 scripts/link_modules.py
  python3 scripts/content_profile.py
fi
# The container build's libraries (Ubuntu 24.04's FFmpeg etc.), as in the AppImage.
if [[ -d $b/AppDir/usr/lib ]]; then export LD_LIBRARY_PATH="$PWD/$b/AppDir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"; fi
# The runtime loads libiss-gpu.so from its own folder.
if [[ -f $b/gpu/libiss-gpu.so && $b/gpu/libiss-gpu.so -nt $b/libiss-gpu.so ]]; then
  cp $b/gpu/libiss-gpu.so $b/libiss-gpu.so
fi
exec $b/iss-probe out/boot-linked.bin --entry "$@"
