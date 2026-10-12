#!/bin/bash
# Inside the container: builds one source tree for measuring. RelWithDebInfo, so it runs at release speed
# and callgrind can still name functions. Usage: build.sh SRC_DIR BUILD_DIR
set -euo pipefail
# main and the branch build from different folders; this lets them share cached objects.
export CCACHE_BASEDIR=${CCACHE_BASEDIR:-/src} CCACHE_NOHASHDIR=1
src=$1
out=$2
cmake -S "$src" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMIRA_GLIBC_COMPAT=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache >/dev/null
cmake --build "$out" --target mirad mira-run mira-gui
