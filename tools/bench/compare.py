#!/usr/bin/env python3
"""Compares a base build's results with a branch's. Writes a short summary of what changed beyond noise
(stdout and --summary, for the PR comment) and every value, scenario by scenario (--full).

Each side takes one or more result files, in run order; repeated runs are shown as their median and
compared run by run. A scenario that fails only on the base is new or changed in the branch, so it's listed
without a comparison. Exits 1 only for a scenario that failed in every branch run and in no base run, so a
broken runner (both sides) or a one-off timeout doesn't fail the check.
Usage: compare.py --base a.json [...] --branch b.json [...] --summary s.md --full f.md
"""

import argparse
import json
import statistics
import sys
from pathlib import Path

# The summary lists what a user would feel, time and memory; instruction counts are in the full log only, since
# they move with code that takes no longer. Short timings on a shared runner step by whole poll intervals (5 to
# 10 ms) from run to run, so a change needs a size, a floor and every run on its side.
TIME_CHANGE = 0.20
TIME_FLOOR_MS = 5.0
MEMORY_CHANGE = 0.10
MEMORY_FLOOR_KB = 2048
IDLE_CPU_FLOOR_MS = 50
# Below this many runs a side, a change is only run again, never listed.
MIN_RUNS = 3
PROCESSES = {"mirad": "mirad", "gui": "the GUI"}
MEMORY = {"threads": "threads", "rss_kb": "resident memory", "pss_anon_kb": "own memory"}


def load(paths):
    """{(mode, scenario, key): [value per run]}, {scenario: [error per failed run]}, {scenario: runs} and the runs."""
    runs = [json.loads(Path(p).read_text()) for p in paths]
    values, failures, ran = {}, {}, {}
    for run in runs:
        for scenario, results in run["results"].items():
            for key, value in results.items():
                values.setdefault((run["mode"], scenario, key), []).append(value)
        for scenario, error in run["failures"].items():
            failures.setdefault(scenario, []).append(error)
        for scenario in {**run["results"], **run["failures"]}:
            ran[scenario] = ran.get(scenario, 0) + 1
    return values, failures, ran, runs


def change(old, new):
    return (new - old) / old if old else 0.0


def paired_change(old_runs, new_runs):
    """Run i of each side ran at the same moment, so they share the machine's load: the median of each
    pair's change holds where the change between medians follows whichever runs were slow."""
    if len(old_runs) == len(new_runs) > 1 and all(old_runs):
        return statistics.median(change(o, n) for o, n in zip(old_runs, new_runs))
    return change(statistics.median(old_runs), statistics.median(new_runs))


def ms(v):
    return f"{v:,.1f} ms"


def mega(v):
    return f"{v / 1e6:,.1f}M"


def size(kb):
    return f"{kb / 1024:,.1f} MB"


def memory_text(metric, v):
    return f"{v:,.0f}" if metric == "threads" else size(v)


def pct(c):
    return (f"{c:+.0%}" if abs(c) >= 0.01 else f"{c:+.2%}").replace("-", "−")


class Report:
    def __init__(self, base_runs, branch_runs):
        self.base_runs, self.branch_runs = base_runs, branch_runs
        self.base = {k: statistics.median(v) for k, v in base_runs.items()}
        self.branch = {k: statistics.median(v) for k, v in branch_runs.items()}
        self.scenarios = list(dict.fromkeys(s for _, s, _ in list(self.branch) + list(self.base)))
        # Scenarios with a change over its threshold, which a few runs can show by chance; run.sh runs them
        # again, and the change is listed only if every run then agrees.
        self.noisy = set()

    def pair(self, mode, scenario, key):
        return self.base.get((mode, scenario, key)), self.branch.get((mode, scenario, key))

    def time_change(self, scenario, key):
        """The change in a measure from the timed runs, memory included."""
        return paired_change(self.base_runs[("time", scenario, key)], self.branch_runs[("time", scenario, key)])

    def keys(self, scenario):
        return list(dict.fromkeys(key for _, s, key in list(self.branch) + list(self.base) if s == scenario))

    @staticmethod
    def is_memory(key):
        return key.partition(".")[0] in PROCESSES

    def moved(self, scenario, key, threshold, floor):
        """(change, base, branch) when a timed measure moved by at least `threshold` and `floor`, else None.
        Over both, the scenario is run again; it's returned only once every branch run is past every base run,
        which load on the runner doesn't do to one side alone."""
        old_runs = self.base_runs.get(("time", scenario, key))
        new_runs = self.branch_runs.get(("time", scenario, key))
        if not old_runs or not new_runs:
            return None
        old, new = statistics.median(old_runs), statistics.median(new_runs)
        c = paired_change(old_runs, new_runs) if old else 1.0
        if abs(c) < threshold or abs(new - old) < floor or (c > 0) != (new > old):
            return None
        self.noisy.add(scenario)
        if min(len(old_runs), len(new_runs)) < MIN_RUNS:
            return None
        held = min(new_runs) > max(old_runs) if c > 0 else max(new_runs) < min(old_runs)
        return (c, old, new) if held else None

    def changes(self):
        """Time, idle CPU and memory changes that held, as (change, line, is memory)."""
        found = []
        for scenario in self.scenarios:
            for key in self.keys(scenario):
                if self.is_memory(key):
                    process, _, metric = key.partition(".")
                    # Thread counts follow pools starting and retiring idle workers on timers, not the code.
                    if metric == "threads":
                        continue
                    if moved := self.moved(scenario, key, MEMORY_CHANGE, MEMORY_FLOOR_KB):
                        c, old, new = moved
                        found.append((c, f"**{scenario}**: {PROCESSES[process]} {MEMORY[metric]} "
                                         f"{memory_text(metric, old)} → {memory_text(metric, new)} ({pct(c)})", True))
                elif key.startswith("idle CPU"):
                    if moved := self.moved(scenario, key, 0, IDLE_CPU_FLOOR_MS):
                        c, old, new = moved
                        found.append((c, f"**{scenario}** idle CPU: {ms(old)} → {ms(new)} in 5 s", False))
                elif moved := self.moved(scenario, key, TIME_CHANGE, TIME_FLOOR_MS):
                    c, old, new = moved
                    found.append((c, f"**{key}** ({scenario}): {ms(old)} → {ms(new)} ({pct(c)})", False))
        return found

    def summary(self):
        """(worse, faster, memory drops) lines, largest change first."""
        found = self.changes()
        worse = [line for c, line, _ in sorted(found, key=lambda f: -f[0]) if c > 0]
        faster = [line for c, line, memory in sorted(found, key=lambda f: f[0]) if c < 0 and not memory]
        smaller = [line for c, line, memory in sorted(found, key=lambda f: f[0]) if c < 0 and memory]
        return worse, faster, smaller

    def full(self, gui_runs):
        once = (" The GUI scenarios ran once, so their values are from that run, unless the summary lists them as "
                "run again." if gui_runs == 1 else "")
        out = ["# Benchmark: every value", "",
               f"Time is the median of the runs, and its change the median of each pair of runs' change, since "
               f"a pair ran at the same moment; instructions are from one callgrind run.{once} Memory is measured "
               "at the end of each scenario.", ""]
        for scenario in self.scenarios:
            out += [f"## {scenario}", "", "| Measure | Time base | Time branch | Change | Instr base | Instr branch | Change |",
                    "|---|---:|---:|---:|---:|---:|---:|"]
            memory_rows = []
            for key in self.keys(scenario):
                t_old, t_new = self.pair("time", scenario, key)
                if self.is_memory(key):
                    process, _, metric = key.partition(".")
                    cell = lambda v: "-" if v is None else memory_text(metric, v)
                    c = pct(self.time_change(scenario, key)) if t_old and t_new is not None else ""
                    memory_rows.append(f"| {PROCESSES[process]} {MEMORY[metric]} | {cell(t_old)} | {cell(t_new)} | {c} |")
                    continue
                i_old, i_new = self.pair("instr", scenario, key)
                tc = pct(self.time_change(scenario, key)) if t_old and t_new is not None else ""
                ic = pct(change(i_old, i_new)) if i_old and i_new is not None else ""
                show_t = lambda v: "-" if v is None else ms(v)
                show_i = lambda v: "-" if v is None else mega(v)
                out.append(f"| {key} | {show_t(t_old)} | {show_t(t_new)} | {tc} | {show_i(i_old)} | {show_i(i_new)} | {ic} |")
            if memory_rows:
                out += ["", "| Memory | Base | Branch | Change |", "|---|---:|---:|---:|", *memory_rows]
            out.append("")
        return "\n".join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", nargs="+", required=True)
    parser.add_argument("--branch", nargs="+", required=True)
    parser.add_argument("--summary")
    parser.add_argument("--full")
    parser.add_argument("--fixture", help="the fixture's JSON, for the summary's first line")
    parser.add_argument("--base-name", default="main")
    parser.add_argument("--recheck", help="writes the scenarios to run again, one a line")
    args = parser.parse_args()
    base, base_failed, _, _ = load(args.base)
    branch, branch_failed, branch_ran, branch_runs = load(args.branch)
    report = Report(base, branch)
    # Runs per scenario: the passes over every scenario, then run.sh's reruns of the noisy ones.
    counts = {}
    for r in branch_runs:
        if r["mode"] == "time":
            for name in {**r["results"], **r["failures"]}:
                counts[name] = counts.get(name, 0) + 1
    passes = lambda gui: min((n for name, n in counts.items() if name.startswith("gui") == gui), default=0)
    time_runs, gui_runs = passes(False), passes(True)
    rechecked = {name: n for name, n in counts.items() if n > passes(name.startswith("gui"))}
    # Broken by the branch: failed every time it ran there and never on the base.
    broken = sorted(name for name, errors in branch_failed.items()
                    if len(errors) == branch_ran[name] and name not in base_failed)

    lines = [f"## Benchmark against {args.base_name}", ""]
    if args.fixture:
        meta = json.loads(Path(args.fixture).read_text())
        once = " The GUI's values are from that one run, not a median, unless run again." if gui_runs == 1 else ""
        lines += [f"{meta['games']} games over {len(meta['sources'])} sources, {meta['titles']:,} store titles; "
                  f"{time_runs} timed runs of mirad's scenarios and {gui_runs} of the GUI's, plus one callgrind run "
                  f"per side.{once}", ""]
    if rechecked:
        lines += ["Run again, since a change looked over its threshold or a run failed: "
                  + ", ".join(f"{name} ({n} runs)" for name, n in sorted(rechecked.items())) + ".", ""]
    if branch_failed:
        def failed(name, errors):
            where = ((f"failed in all {len(errors)} runs" if len(errors) > 1 else "failed in its one run")
                     if name in broken else
                     f"also fails on {args.base_name}" if name in base_failed else
                     f"failed in {len(errors)} of {branch_ran[name]} runs")
            return f"- **{name}** ({where}): {errors[-1]}"
        lines += ["### Failed on the branch", *[failed(name, errors) for name, errors in branch_failed.items()], ""]
    new = sorted(set(base_failed) - set(branch_failed))
    if new:
        lines += [f"Failed on {args.base_name} only, so new or changed here: {', '.join(new)}", ""]
    worse, faster, memory = report.summary()
    for title, items in (("Worse", worse), ("Faster", faster), ("Memory", memory)):
        if items:
            lines += [f"### {title}", *[f"- {item}" for item in items], ""]
    if not (worse or faster or memory or branch_failed):
        lines += ["No change beyond noise.", ""]
    lines.append(f"Listed when every run agrees: time changes over {TIME_CHANGE:.0%} and {TIME_FLOOR_MS:.0f} ms, "
                 f"memory over {MEMORY_CHANGE:.0%} and {MEMORY_FLOOR_KB // 1024} MB, idle CPU over "
                 f"{IDLE_CPU_FLOOR_MS} ms. Instruction counts and every other value are in the full log.")
    summary = "\n".join(lines)
    if args.recheck:
        Path(args.recheck).write_text("".join(f"{name}\n" for name in sorted(report.noisy | set(branch_failed))))
    print(summary)
    if args.summary:
        Path(args.summary).write_text(summary + "\n")
    if args.full:
        Path(args.full).write_text(report.full(gui_runs))
    sys.exit(1 if broken else 0)


if __name__ == "__main__":
    main()
