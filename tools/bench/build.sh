#!/bin/bash
# Inside the container: builds one source tree for measuring. RelWithDebInfo, so it runs at release speed
# and callgrind can still name functions. Usage: build.sh SRC_DIR BUILD_DIR
set -euo pipefail
# Trees build from different folders (main's copy, each checkout); paths relative to the build dir let
# them share cached objects. Natively that's every absolute path, as checkouts can sit anywhere.
export CCACHE_BASEDIR=${CCACHE_BASEDIR:-$([ -n "${MIRA_BENCH_NATIVE:-}" ] && echo / || echo /src)}
export CCACHE_NOHASHDIR=1
# The project's own launcher carries this, but the explicit launcher below replaces it; without it
# every file using the precompiled headers misses.
export CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
# Natively, a cache of its own: the shared one is full of build/dev's debug objects and evicts these.
if [ -n "${MIRA_BENCH_NATIVE:-}" ]; then
  export CCACHE_DIR=${CCACHE_DIR:-$HOME/.cache/mira-bench-ccache} CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-8G}
fi
src=$1
out=$2
cmake -S "$src" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMIRA_GLIBC_COMPAT=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache >/dev/null
cmake --build "$out" --target mirad mira-run mira-gui
