set working-directory := "pc-port"

# Build in the container (Ubuntu 24.04) into pc-port/build, AppImage included
build:
    scripts/linux/container.sh scripts/linux/container-build.sh

test: build
    scripts/linux/container.sh sh -c 'cd build && ctest --output-on-failure'

# Prepare a game folder (the one with eboot.bin), once per folder
setup game:
    scripts/linux/setup.sh "{{game}}"

play: build
    scripts/linux/play.sh

launcher: build
    LD_LIBRARY_PATH=build/AppDir/usr/lib build/iss-launcher

# Retrain PGO: play the slow scenes for a few minutes, quit, then `just build`
pgo:
    rm -f pgo/*.gcda
    scripts/linux/container.sh scripts/linux/container-build.sh build-pgo -DISS_PGO=generate
    ISS_BUILD=build-pgo ISS_USER_DIR="$PWD/build/user" scripts/linux/play.sh  # the saves of build/
