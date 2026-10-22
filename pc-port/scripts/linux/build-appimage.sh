#!/usr/bin/env bash
# Packs the built port into one file to hand out: <build>/inFAMOUS_Second_Son-x86_64.AppImage.
# Build first (cmake; no game needed). Starting the AppImage opens the launcher, where the player
# picks their own unpacked copy of the game.
#   scripts/linux/build-appimage.sh [build folder, default build]
# Run by container-build.sh (just build); an AppImage runs only where glibc is at least as new as
# where it was built, hence the Ubuntu 24.04 container.
# No game files go in: on the first PLAY, play.sh prepares the chosen game folder into
# ~/.local/share/iss-pc/appimage/out (needs python3). linuxdeploy is downloaded to <build>/tools/ on
# the first run (or set LINUXDEPLOY).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
b=${1:-build}
for f in $b/iss-launcher $b/iss-probe; do
  [[ -f $f ]] || { echo "$f is missing: build the port first" >&2; exit 1; }
done
gpu=$b/libiss-gpu.so
[[ -f $b/gpu/libiss-gpu.so && ( ! -f $gpu || $b/gpu/libiss-gpu.so -nt $gpu ) ]] && gpu=$b/gpu/libiss-gpu.so
[[ -f $gpu ]] || { echo "libiss-gpu.so is missing: build with -DISS_BUILD_GPU=ON" >&2; exit 1; }

linuxdeploy=${LINUXDEPLOY:-$b/tools/linuxdeploy-x86_64.AppImage}
if [[ ! -x $linuxdeploy ]]; then
  mkdir -p $b/tools
  curl -fL -o "$linuxdeploy" \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
  chmod +x "$linuxdeploy"
fi

# The AppDir keeps the source tree's layout under usr/: build/, launcher/assets/, scripts/ and the
# shadPS4 symbol table prepare.py reads.
app=$b/AppDir
aerolib=third_party/shadPS4/src/core/aerolib
rm -rf "$app"
mkdir -p "$app/usr/build" "$app/usr/launcher" "$app/usr/$aerolib" "$app/usr/lib"
cp $b/iss-launcher $b/iss-probe "$app/usr/build/"
cp "$gpu" "$app/usr/build/libiss-gpu.so"
cp -r launcher/assets "$app/usr/launcher/"
cp -r scripts "$app/usr/"
find "$app/usr/scripts" -name __pycache__ -prune -exec rm -rf {} +
cp "$aerolib/aerolib.inl" "$app/usr/$aerolib/"

cat > $b/iss-pc.desktop <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=inFAMOUS Second Son
Comment=Settings and launcher for the inFAMOUS Second Son PC port
Exec=iss-launcher
Icon=iss-pc
Categories=Game;
StartupWMClass=iss-launcher
DESKTOP

# The icon: a plain square (the game's own icon is Sony's art, so it stays out).
python3 - $b/iss-pc.png <<'PY'
import struct, sys, zlib
size = 256
row = b"\0" + bytes((40, 40, 48)) * size
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(row * size)) + chunk(b"IEND", b""))
PY

cat > $b/AppRun <<'APPRUN'
#!/usr/bin/env bash
here=$(dirname "$(readlink -f "$0")")
# The AppImage is read-only, so the launcher's root is a folder in the user's data dir: copies of
# the scripts, links to everything else, and out/ for the prepared game image.
root="${XDG_DATA_HOME:-$HOME/.local/share}/iss-pc/appimage"
mkdir -p "$root"
rm -rf "$root/scripts" && cp -r "$here/usr/scripts" "$root/"
for d in build launcher third_party; do ln -sfn "$here/usr/$d" "$root/$d"; done
export ISS_ROOT="$root"
export LD_LIBRARY_PATH="$here/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/usr/build/iss-launcher" "$@"
APPRUN
chmod +x $b/AppRun

# linuxdeploy copies the shared libraries the three binaries need into usr/lib (the system's
# libc, GL and Vulkan loader stay on its exclude list) and makes the AppImage.
# Libraries built next to the port (SDL3 in the portable build) are found through ISS_LIB_PATH.
LD_LIBRARY_PATH="${ISS_LIB_PATH:+$ISS_LIB_PATH:}${LD_LIBRARY_PATH:-}" \
OUTPUT=$b/inFAMOUS_Second_Son-x86_64.AppImage APPIMAGE_EXTRACT_AND_RUN=1 "$linuxdeploy" \
  --appdir "$app" \
  --deploy-deps-only "$app/usr/build/iss-launcher" \
  --deploy-deps-only "$app/usr/build/iss-probe" \
  --deploy-deps-only "$app/usr/build/libiss-gpu.so" \
  --desktop-file $b/iss-pc.desktop --icon-file $b/iss-pc.png \
  --custom-apprun $b/AppRun \
  --output appimage
echo "Made $b/inFAMOUS_Second_Son-x86_64.AppImage"
