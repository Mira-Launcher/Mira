#!/usr/bin/env python3
"""Compares a base build's results with a branch's. Writes a short summary of what changed beyond noise
(stdout and --summary, for the PR comment) and every value, scenario by scenario (--full).

Each side takes one or more result files; repeated runs are reduced to their median. A scenario that fails
only on the base is new or changed in the branch, so it's listed without a comparison.
Usage: compare.py --base a.json [...] --branch b.json [...] --summary s.md --full f.md
"""

import argparse
import json
import statistics
import sys
from pathlib import Path

INSTR_NOISE = 0.02  # callgrind counts barely move between runs
INSTR_FLOOR = 500_000
MEMORY_NOISE = 0.10
WORSE = 0.10  # time and memory rises at least this big are listed
TIME_FLOOR_MS = 1.0
MEMORY_FLOOR_KB = 1024
IDLE_CPU_FLOOR_MS = 50
PROCESSES = {"mirad": "mirad", "gui": "the GUI"}
MEMORY = {"threads": "threads", "rss_kb": "resident memory", "pss_anon_kb": "own memory"}


def load(paths):
    """{(mode, scenario, key): median}, the failures and the runs."""
    runs = [json.loads(Path(p).read_text()) for p in paths]
    values = {}
    for run in runs:
        for scenario, results in run["results"].items():
            for key, value in results.items():
                values.setdefault((run["mode"], scenario, key), []).append(value)
    failures = {}
    for run in runs:
        failures.update(run["failures"])
    return {k: statistics.median(v) for k, v in values.items()}, failures, runs


def change(old, new):
    return (new - old) / old if old else 0.0


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
    def __init__(self, base, branch):
        self.base, self.branch = base, branch
        self.scenarios = list(dict.fromkeys(s for _, s, _ in list(branch) + list(base)))

    def pair(self, mode, scenario, key):
        return self.base.get((mode, scenario, key)), self.branch.get((mode, scenario, key))

    def keys(self, scenario):
        return list(dict.fromkeys(key for _, s, key in list(self.branch) + list(self.base) if s == scenario))

    @staticmethod
    def is_memory(key):
        return key.partition(".")[0] in PROCESSES

    @staticmethod
    def summarized(scenario, metric):
        # With the GUI up, Qt's thread pool and mirad's HTTP lane start and retire idle workers on 30 s
        # timers, so their thread counts follow timing, not the code. The full log still has them.
        return not (metric == "threads" and scenario.startswith("gui"))

    def worse(self):
        """Every measure that got worse beyond its threshold, largest first. If none did but something got
        worse at all, the largest such change, so a regression is never left out."""
        found = []  # (change, line, over the threshold)
        for scenario in self.scenarios:
            for key in self.keys(scenario):
                t_old, t_new = self.pair("time", scenario, key)
                timed = t_old is not None and t_new is not None
                if self.is_memory(key):
                    process, _, metric = key.partition(".")
                    c = change(t_old, t_new) if timed and self.summarized(scenario, metric) else 0
                    if c > 0:
                        floor = 2 if metric == "threads" else MEMORY_FLOOR_KB
                        found.append((c, f"**{scenario}**: {PROCESSES[process]} {MEMORY[metric]} "
                                         f"{memory_text(metric, t_old)} → {memory_text(metric, t_new)} ({pct(c)})",
                                      c >= WORSE and t_new - t_old >= floor))
                    continue
                if key.startswith("idle CPU"):
                    if timed and t_new > t_old:
                        found.append((change(t_old, t_new) if t_old else 1.0,
                                      f"**{scenario}** idle CPU: {ms(t_old)} → {ms(t_new)} in 5 s",
                                      t_new - t_old >= IDLE_CPU_FLOOR_MS))
                    continue
                times = f"{ms(t_old)} → {ms(t_new)}" if timed else ""
                i_old, i_new = self.pair("instr", scenario, key)
                if i_old and i_new is not None and i_new > i_old:
                    c = change(i_old, i_new)
                    over = c >= INSTR_NOISE and i_new - i_old >= INSTR_FLOOR
                    found.append((c, f"**{key}** ({scenario}): {mega(i_old)} → {mega(i_new)} instructions "
                                     f"({pct(c)})" + (f", {times}" if times else ""), over))
                    if over:
                        continue
                # Time alone swings with load on the runner, so it needs a bigger change to count.
                c = change(t_old, t_new) if timed else 0
                if c > 0:
                    found.append((c, f"**{key}** ({scenario}): {times} ({pct(c)})",
                                  c >= WORSE and t_new - t_old >= TIME_FLOOR_MS))
        found.sort(key=lambda f: -f[0])
        lines = [line for _, line, over in found if over]
        if found and not lines:
            lines.append(f"{found[0][1]}, the largest regression, under the thresholds")
        return lines

    def faster(self):
        """Instruction counts that dropped beyond the noise."""
        lines = []
        for scenario in self.scenarios:
            for key in self.keys(scenario):
                i_old, i_new = self.pair("instr", scenario, key)
                if self.is_memory(key) or not i_old or i_new is None:
                    continue
                c = change(i_old, i_new)
                if c <= -INSTR_NOISE and i_old - i_new >= INSTR_FLOOR:
                    t_old, t_new = self.pair("time", scenario, key)
                    times = f", {ms(t_old)} → {ms(t_new)}" if t_old is not None and t_new is not None else ""
                    lines.append(f"**{key}** ({scenario}): {mega(i_old)} → {mega(i_new)} instructions ({pct(c)}){times}")
        return lines

    def memory(self):
        """One line per process and metric that dropped beyond noise, naming the scenario it dropped most in.
        Rises are listed one by one under Worse."""
        lines = []
        for process, name in PROCESSES.items():
            for metric, label in MEMORY.items():
                key = f"{process}.{metric}"
                moved = []
                measured = 0
                for scenario in self.scenarios:
                    old, new = self.pair("time", scenario, key)
                    if old is None or new is None or not self.summarized(scenario, metric):
                        continue
                    measured += 1
                    c = change(old, new)
                    floor = 2 if metric == "threads" else MEMORY_FLOOR_KB
                    if c <= -MEMORY_NOISE and old - new >= floor:
                        moved.append((c, scenario, old, new))
                if moved:
                    c, scenario, old, new = min(moved)
                    lines.append(f"{name} {label} down in {len(moved)} of {measured} scenarios, most in "
                                 f"**{scenario}**: {memory_text(metric, old)} → {memory_text(metric, new)} ({pct(c)})")
        return lines

    def full(self, gui_runs):
        once = " The GUI scenarios ran once, so their values are from that run." if gui_runs == 1 else ""
        out = ["# Benchmark: every value", "",
               f"Time is the median of the runs; instructions are from one callgrind run.{once} Memory is measured "
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
                    c = pct(change(t_old, t_new)) if t_old and t_new is not None else ""
                    memory_rows.append(f"| {PROCESSES[process]} {MEMORY[metric]} | {cell(t_old)} | {cell(t_new)} | {c} |")
                    continue
                i_old, i_new = self.pair("instr", scenario, key)
                tc = pct(change(t_old, t_new)) if t_old and t_new is not None else ""
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
    args = parser.parse_args()
    base, base_failed, _ = load(args.base)
    branch, branch_failed, branch_runs = load(args.branch)
    report = Report(base, branch)
    gui_runs = sum(1 for r in branch_runs if any(s.startswith("gui") for s in {**r["results"], **r["failures"]}))

    lines = [f"## Benchmark against {args.base_name}", ""]
    if args.fixture:
        meta = json.loads(Path(args.fixture).read_text())
        time_runs = sum(1 for r in branch_runs if r["mode"] == "time" and "startup" in r["results"])
        once = " The GUI's values are from that one run, not a median." if gui_runs == 1 else ""
        lines += [f"{meta['games']} games over {len(meta['sources'])} sources, {meta['titles']:,} store titles; "
                  f"{time_runs} timed runs of mirad's scenarios and {gui_runs} of the GUI's, plus one callgrind run "
                  f"per side.{once}", ""]
    if branch_failed:
        lines += ["### Failed on the branch", *[f"- **{name}**: {error}" for name, error in branch_failed.items()], ""]
    new = sorted(set(base_failed) - set(branch_failed))
    if new:
        lines += [f"Failed on {args.base_name} only, so new or changed here: {', '.join(new)}", ""]
    worse, faster, memory = report.worse(), report.faster(), report.memory()
    for title, items in (("Worse", worse), ("Faster", faster), ("Memory", memory)):
        if items:
            lines += [f"### {title}", *[f"- {item}" for item in items], ""]
    if not (worse or faster or memory or branch_failed):
        lines += ["No change beyond noise.", ""]
    lines.append(f"Listed: instructions over {INSTR_NOISE:.0%} and 0.5M; time and memory rises over {WORSE:.0%}, "
                 f"drops in memory over {MEMORY_NOISE:.0%} and 1 MB. Every value is in the full log.")
    summary = "\n".join(lines)
    print(summary)
    if args.summary:
        Path(args.summary).write_text(summary + "\n")
    if args.full:
        Path(args.full).write_text(report.full(gui_runs))
    sys.exit(1 if branch_failed else 0)


if __name__ == "__main__":
    main()
