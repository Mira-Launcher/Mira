#!/bin/bash
# Benchmarks this checkout against a base (default main). Both builds run this checkout's scenarios at the
# same moment, so load on the machine hits both alike, each on its own copy of the fixture made by the
# base's mirad. The GUI check runs beside the callgrind pass, which load doesn't skew.
# Usage: tools/bench/run.sh [BASE_REF] [TIME_RUNS]. Locally it runs in the mira-bench container, offline;
# MIRA_BENCH_NATIVE=1 runs on the machine itself (CI, where the runner is set up like the container).
# MIRA_BENCH_BASE_BIN and MIRA_BENCH_BRANCH_BIN name folders of built binaries (an extracted AppImage's
# usr/bin) to measure instead of building; CI uses the AppImages it already built.
set -euo pipefail
base=${1:-main}
runs=${2:-3}
repo=$(git rev-parse --show-toplevel)
out=$repo/build/bench/out
cd "$repo"
mkdir -p "$out"
rm -f "$out"/*.json "$out"/*.md

if [ -n "${MIRA_BENCH_NATIVE:-}" ]; then
  root=$repo
  export CCACHE_BASEDIR=$repo
  in_box() { "$@"; }
else
  root=/src
  docker image inspect mira-bench >/dev/null 2>&1 || docker build -t mira-bench tools/bench
  mkdir -p ~/.cache/mira-bench-ccache
  in_box() {
    docker run --rm --network none -u "$(id -u):$(id -g)" -e MIRA_BENCH_HOME -v "$repo:/src" \
      -v ~/.cache/mira-bench-ccache:/ccache mira-bench "$@"
  }
fi
b=$root/build/bench
tools=$root/tools/bench
# A HOME per run that can overlap: the callgrind pass is split in two per side (callgrind runs one thread
# at a time, so this uses the cores), and the GUI check runs beside it.
declare -A home=([base]=/tmp/mira-bench-a [branch]=/tmp/mira-bench-b [base-2]=/tmp/mira-bench-c [branch-2]=/tmp/mira-bench-d
                 [base-gui]=/tmp/mira-bench-e [branch-gui]=/tmp/mira-bench-f)

declare -A bin
if [ -n "${MIRA_BENCH_BASE_BIN:-}" ]; then
  bin=([base]=$MIRA_BENCH_BASE_BIN [branch]=$MIRA_BENCH_BRANCH_BIN)
else
  bin=([base]=$b/base [branch]=$b/branch)
  rm -rf build/bench/base-src && mkdir -p build/bench/base-src
  git archive "$base" | tar -x -C build/bench/base-src
  in_box "$tools/build.sh" "$b/base-src" "$b/base" &
  in_box "$tools/build.sh" "$root" "$b/branch" &
  wait
fi
for h in "${!home[@]}"; do
  MIRA_BENCH_HOME=${home[$h]} in_box python3 "$tools/fixture.py" --mirad "${bin[base]}/mirad" --out "$b/out/fixture-$h.tar" &
done
wait

side() {  # side home out-file scenarios.py args...; a failed scenario is in the results, so it doesn't stop the run
  local s=$1 h=$2 file=$3
  shift 3
  MIRA_BENCH_HOME=${home[$h]} in_box python3 "$tools/scenarios.py" --build "${bin[$s]}" --fixture "$b/out/fixture-$h.tar" \
    --out "$b/out/$file" "$@" || true
}
both() {  # suffix home-suffix scenarios.py args...
  local suffix=$1 h=$2
  shift 2
  side base "base$h" "base-$suffix.json" "$@" &
  side branch "branch$h" "branch-$suffix.json" "$@" &
}
for i in $(seq "$runs"); do
  both "time-$i" "" --mode time
  wait
done
read -ra names <<< "$(python3 tools/bench/scenarios.py --list)"
first=() second=()
for i in "${!names[@]}"; do
  if (( i % 2 )); then second+=("${names[$i]}"); else first+=("${names[$i]}"); fi
done
both instr-1 "" --mode instr --only "${first[@]}"
both instr-2 -2 --mode instr --only "${second[@]}"
both gui -gui --gui
wait

# mirad's scenarios first, then the GUI's, which is the order the report lists them in.
python3 tools/bench/compare.py --base "$out"/base-{time-*,instr-*,gui}.json --branch "$out"/branch-{time-*,instr-*,gui}.json \
  --fixture "$out/fixture-base.json" --base-name "${MIRA_BENCH_BASE_NAME:-$base}" \
  --summary "$out/summary.md" --full "$out/full.md"
