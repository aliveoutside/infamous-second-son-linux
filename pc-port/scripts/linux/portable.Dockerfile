# The build environment for everything (just build, the GitHub workflow): Ubuntu 24.04, so what
# it builds needs only glibc 2.39 and runs on current Ubuntu LTS, Fedora, SteamOS and Arch.
# GCC 14 builds the port (pgo/ is trained with it), clang 19 builds shadPS4.
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc-14 g++-14 clang-19 clang-tools-19 lld-19 cmake ninja-build ccache \
      pkg-config git python3 curl ca-certificates file \
      libavcodec-dev libavutil-dev libswscale-dev libtbb-dev uuid-dev libvulkan-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
      libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgl-dev \
      libdrm-dev libgbm-dev libdecor-0-dev libpulse-dev libasound2-dev libpipewire-0.3-dev \
      libudev-dev libdbus-1-dev libusb-1.0-0-dev \
 && rm -rf /var/lib/apt/lists/* \
 && ln -s /usr/bin/clang-19 /usr/local/bin/clang && ln -s /usr/bin/clang++-19 /usr/local/bin/clang++ \
 && ln -s /usr/bin/clang-scan-deps-19 /usr/local/bin/clang-scan-deps
ENV CC=gcc-14 CXX=g++-14
