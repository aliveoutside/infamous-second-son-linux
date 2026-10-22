#!/usr/bin/env bash
# The build, run inside the container (container.sh): SDL3 from the shadPS4 submodule, then the
# port, then the AppDir/AppImage with the libraries it needs.
#   container-build.sh [build folder, default build] [extra cmake options]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
b=${1:-build}
shift $(( $# ? 1 : 0 ))
export CCACHE_DIR=${CCACHE_DIR:-$root/build/ccache} CCACHE_BASEDIR=$root CCACHE_COMPRESS=1 CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-2G}
export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache

# SDL3: Ubuntu 24.04 has none, so the version shadPS4 pins is built once.
sdl=$root/build/sdl3
if [[ ! -f $sdl/lib/pkgconfig/sdl3.pc ]]; then
  cmake -S third_party/shadPS4/externals/sdl3 -B build/sdl3-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$sdl" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
  cmake --build build/sdl3-build && cmake --install build/sdl3-build
fi
export PKG_CONFIG_PATH=$sdl/lib/pkgconfig CMAKE_PREFIX_PATH=$sdl

# A build folder configured outside the container (another compiler) is reconfigured from
# scratch. Only CMake's own files go: build/user holds saves, trophies and caches.
fresh=()
if [[ -f $b/CMakeCache.txt ]] && ! grep -q "^CMAKE_CXX_COMPILER:.*g++-14$" $b/CMakeCache.txt; then
  echo "$b/ was configured outside the container: reconfiguring"
  fresh=(--fresh)
  rm -rf $b/shadps4  # its own CMake cache names the host's compilers
fi
cmake "${fresh[@]}" -S . -B $b -G Ninja -DISS_BUILD_GPU=ON -DCMAKE_BUILD_RPATH="$sdl/lib" "$@"
cmake --build $b
# The AppDir (play.sh uses its libraries on the host too) and the AppImage.
ISS_LIB_PATH=$sdl/lib scripts/linux/build-appimage.sh $b
