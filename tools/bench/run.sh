#!/bin/bash
# Benchmarks this checkout against a base (default main). Both builds run this checkout's scenarios at the
# same moment, so load on the machine hits both alike, each on its own copy of the fixture made by the
# base's mirad. The GUI scenarios run after the callgrind pass, whose load would skew their times.
# Usage: tools/bench/run.sh [BASE_REF] [TIME_RUNS] [GUI_RUNS], GUI_RUNS defaulting to TIME_RUNS. Locally it runs in the mira-bench container, offline;
# MIRA_BENCH_NATIVE=1 runs on the machine itself (CI, where the runner is set up like the container).
# MIRA_BENCH_BASE_BIN and MIRA_BENCH_BRANCH_BIN name folders of built binaries (an extracted AppImage's
# usr/bin) to measure instead of building; CI uses the AppImages it already built.
# MIRA_BENCH_ONLY="read gui-pages" runs only those scenarios; MIRA_BENCH_GAMES sets the fixture's game count.
# Base's build is kept per commit and build.sh, and the fixtures per base build, fixture scripts and size, so a rerun
# against the same base only builds the branch, incrementally.
set -euo pipefail
shopt -s nullglob
base=${1:-main}
runs=${2:-3}
gui_runs=${3:-$runs}
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
# at a time, so this uses the cores).
declare -A home=([base]=/tmp/mira-bench-a [branch]=/tmp/mira-bench-b [base-2]=/tmp/mira-bench-c [branch-2]=/tmp/mira-bench-d)

declare -A bin
if [ -n "${MIRA_BENCH_BASE_BIN:-}" ]; then
  bin=([base]=$MIRA_BENCH_BASE_BIN [branch]=$MIRA_BENCH_BRANCH_BIN)
else
  sha=$(git rev-parse "$base^{commit}")
  bin=([base]=$b/base-$sha [branch]=$b/branch)
  # The stamp holds build.sh's hash, so a change to how trees are built rebuilds base too.
  recipe=$(sha256sum tools/bench/build.sh | cut -c1-16)
  if [ "$(cat "build/bench/base-$sha/.built" 2>/dev/null)" != "$recipe" ]; then
    # Only the newest base is kept.
    rm -rf build/bench/base-src build/bench/base-*/ && mkdir -p build/bench/base-src
    git archive "$sha" | tar -x -C build/bench/base-src
    base_build=(in_box "$tools/build.sh" "$b/base-src" "${bin[base]}")
  fi
  branch_build=(in_box "$tools/build.sh" "$root" "${bin[branch]}")
  if [ -n "${MIRA_BENCH_NATIVE:-}" ]; then
    # One at a time, so two full-width builds don't fill the memory.
    export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-6}
    [ -z "${base_build+x}" ] || "${base_build[@]}"
    "${branch_build[@]}"
  else
    [ -z "${base_build+x}" ] || "${base_build[@]}" &
    "${branch_build[@]}" &
    wait
  fi
  [ -z "${base_build+x}" ] || echo "$recipe" >"build/bench/base-$sha/.built"
fi
games=${MIRA_BENCH_GAMES:-150}
# Made again only when base's mirad, the scripts that make them or the size change. Only the newest are kept.
host_base=${bin[base]/#$root/$repo}
key=$(cat "$host_base/mirad" tools/bench/fixture.py tools/bench/mirabench.py | sha256sum | cut -c1-16)-$games
if [ ! -e "build/bench/fixtures/$key/.built" ]; then
  rm -rf build/bench/fixtures && mkdir -p "build/bench/fixtures/$key"
  for h in "${!home[@]}"; do
    MIRA_BENCH_HOME=${home[$h]} in_box python3 "$tools/fixture.py" --mirad "${bin[base]}/mirad" \
      --out "$b/fixtures/$key/fixture-$h.tar" --games "$games" &
  done
  wait
  touch "build/bench/fixtures/$key/.built"
fi
cp "build/bench/fixtures/$key/fixture-base.json" "$out/"

read -ra only <<< "${MIRA_BENCH_ONLY:-}"
read -ra all_mirad <<< "$(python3 tools/bench/scenarios.py --list)"
read -ra all_gui <<< "$(python3 tools/bench/scenarios.py --list-gui)"
pick() {  # names... -> those in MIRA_BENCH_ONLY, or all of them without it
  local name
  for name in "$@"; do
    if [ ${#only[@]} -eq 0 ] || [[ " ${only[*]} " == *" $name "* ]]; then echo "$name"; fi
  done
}
mapfile -t names < <(pick "${all_mirad[@]}")
mapfile -t gui_names < <(pick "${all_gui[@]}")

side() {  # side home out-file scenarios.py args...; a failed scenario is in the results, so it doesn't stop the run
  local s=$1 h=$2 file=$3
  shift 3
  MIRA_BENCH_HOME=${home[$h]} in_box python3 "$tools/scenarios.py" --build "${bin[$s]}" \
    --fixture "$b/fixtures/$key/fixture-$h.tar" --out "$b/out/$file" "$@" || true
}
both() {  # suffix home-suffix scenarios.py args...
  local suffix=$1 h=$2
  shift 2
  side base "base$h" "base-$suffix.json" "$@" &
  side branch "branch$h" "branch-$suffix.json" "$@" &
}
if [ ${#names[@]} -gt 0 ]; then
  for i in $(seq "$runs"); do
    both "time-$i" "" --mode time --only "${names[@]}"
    wait
  done
  first=() second=()
  for i in "${!names[@]}"; do
    if (( i % 2 )); then second+=("${names[$i]}"); else first+=("${names[$i]}"); fi
  done
  both instr-1 "" --mode instr --only "${first[@]}"
  [ ${#second[@]} -eq 0 ] || both instr-2 -2 --mode instr --only "${second[@]}"
  wait
fi
# The GUI's measures are times, so they run after callgrind's load.
if [ ${#gui_names[@]} -gt 0 ]; then
  for i in $(seq "$gui_runs"); do
    both "gui-$i" "" --gui --only "${gui_names[@]}"
    wait
  done
fi

# mirad's scenarios first, then the GUI's, which is the order the report lists them in.
python3 tools/bench/compare.py --base "$out"/base-{time-*,instr-*,gui-*}.json --branch "$out"/branch-{time-*,instr-*,gui-*}.json \
  --fixture "$out/fixture-base.json" --base-name "${MIRA_BENCH_BASE_NAME:-$base}" \
  --summary "$out/summary.md" --full "$out/full.md"
