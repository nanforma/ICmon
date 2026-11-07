#!/usr/bin/env bash
# Builds icmon_<version>_amd64.deb in ./dist.  Needs: build-essential cmake ninja-build pkg-config libgtk-4-dev
# libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libsecret-1-dev dpkg-dev
set -euo pipefail
cd "$(dirname "$0")/.."
rm -rf build-deb dist && mkdir -p dist
cmake -S . -B build-deb -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DICMON_BUILD_TESTS=OFF
cmake --build build-deb -j"$(nproc)"
(cd build-deb && cpack -G DEB -B "$PWD/../dist")
ls -l dist/*.deb
echo "Install:  sudo apt install ./dist/$(ls dist | grep '\.deb$' | head -1)"
