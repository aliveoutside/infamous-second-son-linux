#!/usr/bin/env bash
# Prepares a game folder for the build: reads its eboot, modules and param.sfo (never writes to
# it) and writes the data the runtime reads at startup into out/. Run once per game folder.
#   scripts/linux/setup.sh "<game folder>"
set -euo pipefail
[[ $# -eq 1 && -f "$1/eboot.bin" ]] || { echo "usage: $0 <game folder with eboot.bin>" >&2; exit 1; }
game=$(realpath "$1")
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
mkdir -p out && echo "$game" > out/game_dir.txt  # play.sh starts this folder
python3 scripts/prepare.py --game "$game"
python3 scripts/link_modules.py --game "$game"
python3 scripts/content_profile.py
