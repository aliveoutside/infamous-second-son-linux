#!/usr/bin/env bash
# Adds the launcher (build/iss-launcher) to the application menu:
#   ~/.local/share/applications/iss-pc.desktop, with the game's own icon (sce_sys/icon0.png).
# Run again after moving the folder. Remove with: rm ~/.local/share/applications/iss-pc.desktop
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
[[ -x "$root/build/iss-launcher" ]] || { echo "build/iss-launcher is missing: build the port first" >&2; exit 1; }
# The game folder: game_dir in settings.toml, else the one setup.sh recorded.
config="${ISS_SETTINGS:-${XDG_CONFIG_HOME:-$HOME/.config}/iss-pc/settings.toml}"
game=$(sed -n 's/^game_dir *= *"\(.*\)".*/\1/p' "$config" 2>/dev/null | head -n1)
[[ -z "$game" && -f "$root/out/game_dir.txt" ]] && game=$(head -n1 "$root/out/game_dir.txt")
icon=applications-games
if [[ -n "$game" && -f "$game/sce_sys/icon0.png" ]]; then
  mkdir -p "${XDG_DATA_HOME:-$HOME/.local/share}/icons"
  cp "$game/sce_sys/icon0.png" "${XDG_DATA_HOME:-$HOME/.local/share}/icons/iss-pc.png"
  icon="${XDG_DATA_HOME:-$HOME/.local/share}/icons/iss-pc.png"
fi
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
cat > "$apps/iss-pc.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=inFAMOUS Second Son
Comment=Settings and launcher for the inFAMOUS Second Son PC port
Exec="$root/build/iss-launcher"
Icon=$icon
Categories=Game;
StartupWMClass=iss-launcher
DESKTOP
command -v update-desktop-database >/dev/null && update-desktop-database "$apps" >/dev/null 2>&1 || true
echo "Added $apps/iss-pc.desktop"
# Steam Deck, Desktop Mode: a Desktop icon too (Add to Steam in the menu or, for Game Mode, see docs/steam-deck.md).
if [[ -d "$HOME/Desktop" && "$(cat /sys/devices/virtual/dmi/id/board_vendor 2>/dev/null)" == Valve* ]]; then
  cp "$apps/iss-pc.desktop" "$HOME/Desktop/iss-pc.desktop" && chmod +x "$HOME/Desktop/iss-pc.desktop"
  echo "Added $HOME/Desktop/iss-pc.desktop"
fi
