#!/usr/bin/env bash
# Runs a command in the build container (portable.Dockerfile, podman or docker), with the
# repository mounted at its own path. The image is built on first use and cached.
#   scripts/linux/container.sh <command>    (just build, just test)
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
engine=$(command -v podman || command -v docker) || { echo "needs podman or docker" >&2; exit 1; }
[[ -f $root/third_party/shadPS4/CMakeLists.txt ]] || git -C "$root" submodule update --init --recursive
echo "Container image (cached after the first build):"
"$engine" build -t iss-build -f "$root/scripts/linux/portable.Dockerfile" "$root/scripts/linux"
repo=$(git -C "$root" rev-parse --show-toplevel)
args=(--rm -v "$repo:$repo" -w "$root" -e HOME=/tmp -e CCACHE_DIR="$root/build/ccache")
# Rootless podman already maps the container's root to you; docker needs your ids.
[[ $(basename "$engine") == podman ]] && args+=(--security-opt label=disable) || args+=(--user "$(id -u):$(id -g)")
exec "$engine" run "${args[@]}" iss-build "$@"
