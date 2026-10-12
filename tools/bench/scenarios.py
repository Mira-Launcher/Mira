#!/usr/bin/env python3
"""mirad's benchmark scenarios. Each one starts mirad on a fresh copy of the fixture, does something a user
does, checks the answer was right, and records what it cost:

  --mode time   wall time (median of repeats), plus mirad's RSS, anon memory and threads at the end
  --mode instr  instructions executed (callgrind), which barely move between runs, so small changes show

Usage: scenarios.py --build DIR --fixture TAR --mode time|instr --out results.json [--only NAME ...]
"""

import argparse
import contextlib
import json
import os
import re
import signal
import shutil
import statistics
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mirabench import Mirad, callgrind_total, cpu_ticks, proc_status, unpack  # noqa: E402

SCENARIOS = []


def scenario(name, startup=False, gui=False):
    """`startup` scenarios measure mirad's own start, so they get mirad before it's started. `gui` ones
    measure the GUI, so they only run with --gui (time mode, beside the callgrind pass)."""
    def register(fn):
        SCENARIOS.append((name, fn, startup, gui))
        return fn
    return register


class Failed(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failed(message)


class Context:
    def __init__(self, mirad, mode, results):
        self.mirad = mirad
        self.mode = mode
        self.results = results
        self.client = mirad.client()

    @contextmanager
    def measure(self, label, repeats=1, settle=0):
        """Measures the block; `repeats` says how many operations it does, so the result is per operation.
        `settle` seconds of follow-up work (the watcher reacting) are counted too, so it's never half in."""
        if self.mode == "instr":
            self.mirad.callgrind("zero")
            self.mirad.callgrind("on")
            yield
            time.sleep(settle)
            self.mirad.callgrind("off")
            self.mirad.callgrind("dump")
            self.results[label] = latest_dump(self.mirad) // repeats
        else:
            start = time.perf_counter()
            yield
            self.results[label] = round((time.perf_counter() - start) * 1000 / repeats, 3)

    def repeat(self, label, times, fn):
        """`fn` run `times` times; in time mode the median, in instr mode the mean of one run each."""
        if self.mode == "instr":
            with self.measure(label, repeats=times):
                for _ in range(times):
                    fn()
            return
        samples = []
        for _ in range(times):
            start = time.perf_counter()
            fn()
            samples.append((time.perf_counter() - start) * 1000)
        self.results[label] = round(statistics.median(samples), 3)

    def until(self, condition, force=False, timeout=60):
        """Polls GET /v1/games until `condition(games)`. Not in callgrind mode unless forced: a poll costs
        as much as what's measured and their number varies, so the window waits out `settle` instead."""
        if self.mode == "instr" and not force:
            return None
        deadline = time.time() + timeout
        while time.time() < deadline:
            if condition(self.games()):
                return True
            time.sleep(0.02)
        return False

    def games(self):
        status, body = self.client.get("/v1/games")
        check(status == 200, f"GET /v1/games answered {status}")
        return body if isinstance(body, list) else body["games"]


def latest_dump(mirad):
    dumps = sorted(Path(mirad.out_file).parent.glob(Path(mirad.out_file).name + ".*"),
                   key=lambda p: p.stat().st_mtime)
    return callgrind_total(dumps[-1]) if dumps else 0


def expected(fixture_meta, key):
    return fixture_meta[key]


# --- scenarios ------------------------------------------------------------------------------------------------

@scenario("startup", startup=True)
def startup(ctx, meta):
    """mirad from nothing to answering, then to its startup scan done (nothing changed since the fixture)."""
    if ctx.mode == "instr":
        # A dump zeroes the counts, so the second is the scan's share.
        ctx.mirad.start(wait=True)
        ctx.mirad.callgrind("dump")
        ctx.results["until answering"] = latest_dump(ctx.mirad)
        ctx.mirad.wait_log("startup scan:")
        ctx.mirad.callgrind("dump")
        ctx.results["until scanned"] = ctx.results["until answering"] + latest_dump(ctx.mirad)
    else:
        ctx.mirad.start(wait=True)
        ctx.results["until answering"] = round(ctx.mirad.ready_after * 1000, 1)
        ctx.results["until scanned"] = round(ctx.mirad.wait_log("startup scan:") * 1000, 1)
    ctx.client = ctx.mirad.client()
    log = (ctx.mirad.home / "mirad.log").read_text()
    check("startup scan: added 0, missing 0" in log, "the startup scan changed a library nothing touched")
    check(len(ctx.games()) == meta["games"], "not every fixture game is listed after startup")


@scenario("read")
def reads(ctx, meta):
    """What the GUI asks for on open and on every library change."""
    def games():
        check(len(ctx.games()) == meta["games"], "GET /v1/games lost games")
    ctx.repeat("GET /v1/games", 10, games)

    def library():
        status, body = ctx.client.get("/v1/library")
        check(status == 200, f"GET /v1/library answered {status}")
        titles = body if isinstance(body, list) else body.get("titles", body.get("entries", []))
        check(len(titles) >= meta["titles"], f"GET /v1/library listed {len(titles)} of {meta['titles']} titles")
    ctx.repeat("GET /v1/library", 5, library)

    def sources():
        status, body = ctx.client.get("/v1/sources")
        check(status == 200 and len(body["sources"]) > 40, "GET /v1/sources")
    ctx.repeat("GET /v1/sources", 10, sources)

    def emulators():
        status, _ = ctx.client.get("/v1/emulators")
        check(status == 200, "GET /v1/emulators")
    ctx.repeat("GET /v1/emulators", 10, emulators)

    def tags():
        status, body = ctx.client.get("/v1/tags")
        check(status == 200 and len(body["tags"]) >= 5, "GET /v1/tags")
    ctx.repeat("GET /v1/tags", 10, tags)


@scenario("covers")
def covers(ctx, meta):
    """A screenful-and-more of covers, then a few heroes, as a big library scrolls."""
    ids = sorted(g["id"] for g in ctx.games())[:200]

    def fetch_all():
        for game_id in ids:
            status, data = ctx.client.request("GET", f"/v1/games/{game_id}/artwork?type=cover", raw=True)
            check(status == 200 and data[:3] == b"\xff\xd8\xff", f"{game_id}'s cover isn't the JPEG it was given")
    with ctx.measure("200 covers", repeats=1):
        fetch_all()


@scenario("rescan")
def rescan(ctx, meta):
    """Ctrl+R on a library nothing changed in."""
    with ctx.measure("scan, nothing changed"):
        state = ctx.client.wait_job(ctx.client.request("POST", "/v1/library/scan"))
    result = state.get("result", {})
    check(state.get("state") == "finished", f"scan ended {state}")
    check(result.get("added") == 0 and result.get("missing") == 0, f"a scan of an unchanged library changed it: {result}")


@scenario("scan-new")
def scan_new(ctx, meta):
    """100 new game folders arrive, then a scan picks them up."""
    root = ctx.mirad.home / "Mira" / "Games"
    elf = Path(shutil.which("true")).read_bytes()
    # The watcher would take them as they land; a scan in the same moment races it, so it's paused by
    # writing them inside one folder first and moving them in together.
    staging = ctx.mirad.home / "staging"
    for i in range(100):
        folder = staging / f"Bench New {i:03d}"
        folder.mkdir(parents=True)
        exe = folder / f"BenchNew{i:03d}.x86_64"
        exe.write_bytes(elf)
        exe.chmod(0o755)
    for folder in staging.iterdir():
        folder.rename(root / folder.name)
    with ctx.measure("scan, 100 new games"):
        state = ctx.client.wait_job(ctx.client.request("POST", "/v1/library/scan"))
    check(state.get("state") == "finished", f"scan ended {state}")
    deadline = time.time() + 30
    while time.time() < deadline and len(ctx.games()) < meta["games"] + 100:
        time.sleep(0.2)
    check(len(ctx.games()) == meta["games"] + 100, "the 100 new games weren't all added")


@scenario("watcher")
def watcher(ctx, meta):
    """A game folder dropped in, then another deleted, while mirad runs: the watcher alone has to notice."""
    root = ctx.mirad.home / "Mira" / "Games"
    victim = sorted(p for p in root.iterdir() if p.is_dir())[0]
    elf = Path(shutil.which("true")).read_bytes()
    staging = ctx.mirad.home / "staging" / "Bench Dropped"
    staging.mkdir(parents=True)
    (staging / "BenchDropped.x86_64").write_bytes(elf)
    (staging / "BenchDropped.x86_64").chmod(0o755)

    def added(games):
        return any(g["name"] == "Bench Dropped" for g in games)

    def missing(games):
        return any(g["install_path"] == str(victim) and g["status"] == "missing" for g in games)

    with ctx.measure("watcher, folder added", settle=5):
        staging.rename(root / "Bench Dropped")
        ctx.until(added)
    check(ctx.until(added, force=True), "the dropped folder wasn't added")
    with ctx.measure("watcher, folder deleted", settle=5):
        shutil.rmtree(victim)
        ctx.until(missing)
    check(ctx.until(missing, force=True), "the deleted game wasn't marked missing")


@scenario("emulators")
def emulators(ctx, meta):
    """Importing an emulator's folder again, which every scan does for each added emulator."""
    with ctx.measure("mGBA import, nothing changed"):
        state = ctx.client.wait_job(ctx.client.request("POST", "/v1/emulators/mgba/import"))
    check(state.get("state") == "finished" and state["result"].get("added") == 0, f"re-import changed things: {state}")
    status, plan = ctx.client.get("/v1/sources/mesen/removal")
    check(status == 200 and len(plan.get("games", [])) > 0, "Mesen's removal plan lists no games")
    ctx.repeat("Mesen removal plan", 5, lambda: ctx.client.get("/v1/sources/mesen/removal"))


@scenario("edits")
def edits(ctx, meta):
    """Editing games: one at a time, many at once by tag, and a tag set across the library."""
    ids = sorted(g["id"] for g in ctx.games())

    def rename(i=[0]):
        game_id = ids[i[0] % len(ids)]
        i[0] += 1
        status, _ = ctx.client.request("PATCH", f"/v1/games/{game_id}", {"name": f"Renamed {i[0]}"})
        check(status == 200, f"PATCH /v1/games/{game_id} answered {status}")
    ctx.repeat("rename one game", 20, rename)

    batch = ids[: len(ids) // 2]
    with ctx.measure("tag half the library"):
        status, _ = ctx.client.request("PATCH", "/v1/games", {"ids": batch, "add_tags": ["Bench"]})
    check(status == 200, f"PATCH /v1/games answered {status}")
    _, tags = ctx.client.get("/v1/tags")
    bench = next((t for t in tags["tags"] if t["name"] == "Bench"), None)
    check(bench is not None and bench["count"] == len(batch), "the batch tag isn't on exactly the games given")

    with ctx.measure("set a tag on 100 games"):
        status, _ = ctx.client.request("POST", "/v1/tags/set", {"name": "Bench Set", "ids": ids[:100]})
    check(status == 200, f"POST /v1/tags/set answered {status}")


@scenario("events")
def events(ctx, meta):
    """Eight clients on the event stream while a burst of game changes goes out."""
    sock = str(ctx.mirad.sock)
    files = [ctx.mirad.home / f"events{i}.txt" for i in range(8)]
    readers = [subprocess.Popen(["curl", "-sN", "--unix-socket", sock, "http://mirad/v1/events"],
                                stdout=path.open("wb"), stderr=subprocess.DEVNULL) for path in files]
    time.sleep(0.5)
    ids = sorted(g["id"] for g in ctx.games())[:50]
    with ctx.measure("50 changes to 8 listeners", repeats=1):
        for i, game_id in enumerate(ids):
            ctx.client.request("PATCH", f"/v1/games/{game_id}", {"name": f"Event {i}"})
    time.sleep(0.5)
    for reader in readers:
        reader.terminate()
        reader.wait()
    # Changes go out batched as games.updated, so each listener is checked for every new name.
    outputs = [path.read_bytes() for path in files]
    seen = [len(set(re.findall(rb'"name":"Event \d+"', out))) for out in outputs]
    check(all(n == 50 for n in seen), f"a listener missed changes: {seen}")


@scenario("relocate")
def relocate(ctx, meta):
    """Moving a game's folder to another place in the library."""
    game = next(g for g in sorted(ctx.games(), key=lambda g: g["id"]) if g.get("platform") == "native")
    target = str(ctx.mirad.home / "Mira" / "Games" / "Moved" / Path(game["install_path"]).name)
    with ctx.measure("move one game", settle=4):
        state = ctx.client.wait_job(ctx.client.request("POST", f"/v1/games/{game['id']}/relocate",
                                                       {"install_path": target}))
    check(state.get("state") == "finished", f"relocate ended {state}")
    check(Path(target).is_dir(), "the game's folder isn't at its new place")


def gui_pid(launcher, timeout=30):
    """mira-gui's own pid in the session xvfb-run started."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        check(launcher.poll() is None, f"xvfb-run exited with {launcher.returncode}")
        for stat in Path("/proc").glob("[0-9]*/stat"):
            try:
                fields = stat.read_text().rsplit(")", 1)
                name = fields[0].split("(", 1)[1]
                session = int(fields[1].split()[3])
            except (OSError, IndexError, ValueError):
                continue
            if name == "mira-gui" and session == launcher.pid:
                return int(stat.parent.name)
        time.sleep(0.02)
    raise Failed("mira-gui never started")


def open_gui(ctx, label, args=()):  # label names its log
    """The real GUI, offscreen, on the fixture: time until it goes quiet (library and art loaded), then its
    CPU while idle, which catches redraw loops, and its memory and threads."""
    # On Xvfb rather than Qt's offscreen platform, which the AppImage doesn't bundle.
    env = ctx.mirad.env() | {"QT_QPA_PLATFORM": "xcb"}
    # A build tree has it under frontend/; an AppImage beside mirad.
    folder = Path(ctx.mirad.binary).parent
    binary = next(p for p in (folder / "frontend" / "mira-gui", folder / "mira-gui") if p.exists())
    log = open(ctx.mirad.home / f"{label}.log", "w")
    gui = subprocess.Popen(["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24", "--", str(binary), *args],
                           env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        start = time.perf_counter()
        pid = gui_pid(gui)
        quiet_since, last, deadline = None, -1, start + 60
        while time.perf_counter() < deadline:
            check(gui.poll() is None, f"the GUI exited with {gui.returncode}")
            ticks = cpu_ticks(pid)
            if ticks != last:
                quiet_since, last = None, ticks
            elif quiet_since is None:
                quiet_since = time.perf_counter()
            elif time.perf_counter() - quiet_since >= 1.0 and ticks > 0:
                break
            time.sleep(0.05)
        check(quiet_since is not None, "the GUI never went quiet")
        ctx.results["until quiet"] = round((quiet_since - start) * 1000, 1)
        before = cpu_ticks(pid)
        time.sleep(5)
        idle = cpu_ticks(pid) - before
        ctx.results["idle CPU ms per 5 s"] = idle * 10
        ctx.results.update({f"gui.{k}": v for k, v in proc_status(pid).items()})
        check(idle <= 10, f"the GUI used {idle * 10} ms of CPU in 5 idle seconds (a redraw loop?)")
        check(gui.poll() is None, f"the GUI exited with {gui.returncode}")
    except Failed as e:
        # Its last lines say why, such as a library the runner lacks.
        log.flush()
        tail = (ctx.mirad.home / f"{label}.log").read_text(errors="replace").strip().splitlines()[-3:]
        raise Failed(" / ".join([str(e), *tail])) from None
    finally:
        with contextlib.suppress(ProcessLookupError):  # the session may already be gone
            os.killpg(gui.pid, signal.SIGTERM)
            try:
                gui.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(gui.pid, signal.SIGKILL)
        log.close()


@scenario("gui", gui=True)
def gui_library(ctx, meta):
    """Opening Mira on the library."""
    open_gui(ctx, "GUI library")


@scenario("gui-big-screen", gui=True)
def gui_big_screen(ctx, meta):
    """Opening Mira straight into big screen."""
    open_gui(ctx, "GUI big screen", ["--big-screen"])


# --- runner ---------------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build")
    parser.add_argument("--fixture")
    parser.add_argument("--mode", choices=("time", "instr"), default="time")
    parser.add_argument("--out")
    parser.add_argument("--only", nargs="*")
    parser.add_argument("--gui", action="store_true", help="run the GUI scenarios instead of mirad's")
    parser.add_argument("--list", action="store_true", help="print mirad's scenarios and stop")
    args = parser.parse_args()
    if args.list:
        print(" ".join(name for name, _, _, is_gui in SCENARIOS if not is_gui))
        return
    meta = json.loads(Path(args.fixture).with_suffix(".json").read_text())
    binary = Path(args.build) / "mirad"
    results, failures = {}, {}
    for name, fn, is_startup, is_gui in SCENARIOS:
        if is_gui != args.gui or (args.only and name not in args.only):
            continue
        home = unpack(args.fixture)
        tool = "callgrind" if args.mode == "instr" and not is_gui else None
        extra = [] if is_startup else ["--instr-atstart=no"]
        mirad = Mirad(binary, home, tool=tool, out_dir=home, extra_valgrind=extra)
        scenario_results = {}
        ctx = Context(mirad, args.mode, scenario_results)
        try:
            if not is_startup:
                mirad.start(timeout=600)
                mirad.wait_log("startup scan:", timeout=1200)
                ctx.client = mirad.client()
            fn(ctx, meta)
            if args.mode == "time":
                scenario_results.update({f"mirad.{k}": v for k, v in mirad.proc_status().items()})
            print(f"PASS {name}", file=sys.stderr)
        except Exception as error:  # a failed scenario is reported, and the others still run
            failures[name] = f"{type(error).__name__}: {error}"
            print(f"FAIL {name}: {failures[name]}", file=sys.stderr)
        finally:
            mirad.stop()
        results[name] = scenario_results
    Path(args.out).write_text(json.dumps({"mode": args.mode, "results": results, "failures": failures}, indent=1))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
