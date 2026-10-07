#!/bin/sh
# Runs inside the builder image: the source tree is mounted read-only at /src and the
# finished AppImage, .deb and .rpm are copied to /out.
set -eu
BUILD=/tmp/mira-build
cmake -S /src -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
  -DCMAKE_PREFIX_PATH="$QT_ROOT" -DMIRA_GLIBC_COMPAT=OFF -DMIRA_BUNDLE_CXX_RUNTIME=ON
cmake --build "$BUILD" --target appimage
cmake --build "$BUILD" --target packages
cp "$BUILD"/Mira-x86_64.AppImage "$BUILD"/*.deb "$BUILD"/*.rpm /out/
