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
import sqlite3
import statistics
import struct
import subprocess
import sys
import time
from contextlib import closing, contextmanager
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mirabench import Mirad, callgrind_total, cpu_ticks, proc_status, unpack  # noqa: E402

SCENARIOS = []


def scenario(name, startup=False, gui=False):
    """`startup` scenarios measure mirad's own start, so they get mirad before it's started. `gui` ones
    measure the GUI, so they only run with --gui (time mode, after the callgrind pass)."""
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
        """Every game, hidden ones too while they're unlocked."""
        status, body = self.client.get("/v1/games?include_hidden=true")
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
        # Not all: a tag only hidden games have isn't listed.
        check(len(body["steam"]) >= meta.get("steam_tags", 0) // 2,
              f"GET /v1/tags suggested {len(body['steam'])} of {meta.get('steam_tags')} Steam tags")
    ctx.repeat("GET /v1/tags", 10, tags)


@scenario("covers")
def covers(ctx, meta):
    """A screenful-and-more of covers, then a few heroes, as a big library scrolls."""
    ids = sorted(g["id"] for g in ctx.games())[:200]

    def fetch_all():
        for game_id in ids:
            status, data = ctx.client.request("GET", f"/v1/games/{game_id}/artwork?type=cover", raw=True)
            check(status == 200 and data[:3] == b"\xff\xd8\xff", f"{game_id}'s cover isn't the JPEG it was given")
    # One pass is tens of ms, so a single hiccup moved it by 30%: the median of several holds.
    ctx.repeat("200 covers", 7, fetch_all)


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


@scenario("hidden")
def hidden(ctx, meta):
    """Hidden games behind a password: what the GUI reads while they're locked, then unlocking them."""
    status, _ = ctx.client.request("PUT", "/v1/hidden/password", {"password": "bench"})
    check(status == 200, f"PUT /v1/hidden/password answered {status}")
    ctx.client.request("POST", "/v1/hidden/lock")
    shown = meta["games"] - meta["hidden"]

    def games():
        check(len(ctx.games()) == shown, "GET /v1/games, locked, didn't list exactly the games that aren't hidden")
    ctx.repeat("GET /v1/games, locked", 10, games)

    def library():
        status, body = ctx.client.get("/v1/library")
        check(status == 200, f"GET /v1/library answered {status}")
        titles = body if isinstance(body, list) else body.get("titles", body.get("entries", []))
        check(titles and not any(t.get("hidden") for t in titles), "GET /v1/library, locked, listed hidden titles")
    ctx.repeat("GET /v1/library, locked", 5, library)

    def tags():
        status, body = ctx.client.get("/v1/tags")
        check(status == 200 and len(body["tags"]) >= 5, "GET /v1/tags")
    ctx.repeat("GET /v1/tags, locked", 10, tags)

    def unlock():
        ctx.client.request("POST", "/v1/hidden/lock")
        status, body = ctx.client.request("POST", "/v1/hidden/unlock", {"password": "bench"})
        check(status == 200 and not body["locked"], f"POST /v1/hidden/unlock answered {status}")
    ctx.repeat("lock and unlock", 5, unlock)
    check(len(ctx.games()) == meta["games"], "unlocked, GET /v1/games lost games")


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


def save_locations(path, names, entries=40000):
    """A save locations list about the size of the real one: `names` keep their saves in <base>/saves,
    the rest are filler with a Steam id and a prefix folder each."""
    with path.open("w") as out:
        for name in names:
            out.write(f"{json.dumps(name)}:\n  files:\n    \"<base>/saves\":\n      tags: [save]\n")
        for i in range(entries - len(names)):
            out.write(f"\"Filler Game {i}\":\n  files:\n    \"<winAppData>/Filler{i}/save.dat\":\n"
                      f"      tags: [save]\n  steam:\n    id: {100000 + i}\n")


def write_saves(folder, files, total_bytes):
    folder.mkdir(parents=True, exist_ok=True)
    for i in range(files):
        (folder / f"slot{i}.sav").write_bytes(os.urandom(total_bytes // files))


@scenario("saves")
def saves(ctx, meta):
    """The save locations list indexed; 20 games' saves looked up; backups of a typical save set (50 files,
    10 MB) and a large one (5,000 files, 250 MB), and of the typical one unchanged; and a restore."""
    home = ctx.mirad.home
    games = [g for g in sorted(ctx.games(), key=lambda g: g["id"]) if g.get("platform") == "native"][:20]
    check(len(games) == 20, f"the fixture has {len(games)} native games, not 20")
    listing = home / "save-locations.yaml"
    save_locations(listing, [g["name"] for g in games])
    status, body = ctx.client.request("PATCH", "/v1/config", {"saves": {"manifest_url": f"file://{listing}"}})
    check(status == 200, f"setting the list's address answered {status}: {body}")
    with ctx.measure("index the save locations list"):
        state = ctx.client.wait_job(ctx.client.request("POST", "/v1/saves/manifest/update"))
    check(state.get("state") == "finished", f"indexing ended {state}")

    typical, large = games[0], games[1]
    write_saves(Path(typical["install_path"]) / "saves", 50, 10 << 20)
    write_saves(Path(large["install_path"]) / "saves", 5000, 250 << 20)

    def look():
        for game in games:
            status, body = ctx.client.get(f"/v1/games/{game['id']}/saves")
            check(status == 200 and body.get("entry"), f"{game['id']}'s saves answered {status}: {body}")
    with ctx.measure("GET a game's saves", repeats=len(games)):
        look()

    def back_up(game):
        state = ctx.client.wait_job(ctx.client.request("POST", f"/v1/games/{game['id']}/saves/backup"))
        check(state.get("state") == "finished", f"backing up {game['id']} ended {state}")
        return state["result"]
    with ctx.measure("back up 50 files, 10 MB"):
        taken = back_up(typical)
    check(taken.get("taken"), "the typical save set wasn't backed up")
    with ctx.measure("back up 50 files, unchanged"):
        check(not back_up(typical).get("taken"), "an unchanged save set was backed up again")
    with ctx.measure("back up 5,000 files, 250 MB"):
        check(back_up(large).get("taken"), "the large save set wasn't backed up")

    (Path(typical["install_path"]) / "saves" / "slot0.sav").write_bytes(b"changed")
    with ctx.measure("restore 50 files"):
        state = ctx.client.wait_job(ctx.client.request("POST", f"/v1/games/{typical['id']}/saves/restore",
                                                       {"snapshot": taken["snapshot"]["id"]}))
    check(state.get("state") == "finished", f"restoring ended {state}")


# --- store games ----------------------------------------------------------------------------------------------
# Epic, GOG and Steam games with stand-in store tools that answer at once, so what's measured is Mira's own
# work. No API adds a game with a store as its source, so they're written into mira.db before mirad starts.

def write_script(path, body):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o755)


def add_games(home, games):
    """`games`: dicts of id, name, source, ref and runner_ref, each a native game running `run.sh` in
    home/store-games/<id>, which notes when it ends."""
    now = int(time.time())
    with closing(sqlite3.connect(home / ".config" / "mira" / "mira.db")) as db, db:
        for game in games:
            folder = home / "store-games" / game["id"]
            write_script(folder / "run.sh", 'date +%s.%N > "$0.end"\n')
            db.execute("INSERT INTO games(id, install_path, name, status, confidence, reviewed, platform, source, exe_path,"
                       " args, working_dir, runner_ref, source_ref, data_dir, installer_dir, last_error, created_at,"
                       " updated_at, play_seconds, last_session_at, runner_config, overrides, env, candidates)"
                       " VALUES(?, ?, ?, 'ready', 1, 1, 'native', ?, 'run.sh', '', '', ?, ?, '', '', '', ?, ?, 0, 0,"
                       " '{}', '{}', '{}', '[]')",
                       (game["id"], str(folder), game["name"], game["source"], game.get("runner_ref", ""),
                        game.get("ref", ""), now, now))


def add_settings(home, text):
    with (home / ".config" / "mira" / "settings.toml").open("a") as settings:
        settings.write("\n" + text)


def fake_stores(home, epic_achievements=None):
    """Signed-in stand-ins for legendary, gogdl and comet. Returns the environment mirad needs to find
    GOG's account name and achievements without the network."""
    tools = home / "fake-tools"
    epic_args = json.dumps({"game_parameters": [], "egl_parameters": [
        "-AUTH_LOGIN=unused", "-AUTH_PASSWORD=bench0code", "-AUTH_TYPE=exchangecode", "-epicapp=Bench",
        "-epicenv=Prod", "-EpicPortal", "-epicusername=Bench", "-epicuserid=bench"]})
    answer = home / "legendary-achievements.json"
    answer.write_text(json.dumps(epic_achievements or {"completed": [], "in_progress": [], "uninitiated": [],
                                                       "hidden": []}))
    write_script(tools / "legendary", 'case "$1" in\n'
                 f"  launch) echo '{epic_args}' ;;\n"
                 f'  achievements) cat "{answer}" ;;\n'
                 "  --version) echo 'legendary version \"0.21.1\"' ;;\n"
                 "esac\n")
    write_script(tools / "gogdl", '[ "$1" = --version ] && echo 1.3.1\n')
    write_script(tools / "comet", "[ \"$1\" = --version ] && echo 'comet 0.3.2' && exit 0\nexec sleep 300\n")
    legendary = home / ".config" / "legendary"
    legendary.mkdir(parents=True, exist_ok=True)
    (legendary / "user.json").write_text('{"displayName": "Bench", "refresh_expires_at": "2999-01-01T00:00:00Z"}')
    (home / ".config" / "mira" / "gog-auth.json").write_text(json.dumps({"46899977096215655": {
        "access_token": "bench", "refresh_token": "bench", "user_id": "4815", "loginTime": int(time.time()),
        "expires_in": 86400}}))
    endpoints = home / "endpoints"
    (endpoints / "GOG_EMBED").mkdir(parents=True, exist_ok=True)
    (endpoints / "GOG_EMBED" / "userData.json").write_text('{"username": "Bench"}')
    gog_list = endpoints / "GOG_GAMEPLAY" / "clients" / "2000" / "users" / "4815" / "achievements"
    gog_list.parent.mkdir(parents=True, exist_ok=True)
    gog_list.write_text('{"total_count": 0, "limit": 1000, "page_token": "MA", "items": []}')
    add_settings(home, f'[epic]\nlegendary_bin = "{tools / "legendary"}"\n\n[gog]\ngogdl_bin = "{tools / "gogdl"}"\n')
    return {"MIRA_GOG_EMBED_URL": f"file://{endpoints / 'GOG_EMBED'}",
            "MIRA_GOG_GAMEPLAY_URL": f"file://{endpoints / 'GOG_GAMEPLAY'}"}


def start_with(ctx, env):
    """Starts a startup scenario's mirad with `env` added, as far as its startup scan."""
    saved = {key: os.environ.get(key) for key in env}
    os.environ.update(env)
    try:
        ctx.mirad.start(timeout=600)
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
    ctx.mirad.wait_log("startup scan:", timeout=1200)
    ctx.client = ctx.mirad.client()
    # Only this branch has comet, so base answers 400 and runs GOG games without it.
    ctx.client.request("PATCH", "/v1/config", {"gog": {"comet_bin": str(ctx.mirad.home / "fake-tools" / "comet")}})


def link_systemd(home):
    """Games start in a systemd scope of their own, so the host's user manager is reached from the bench's
    runtime dir through links to its sockets; mirad's own socket stays in the bench."""
    host = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    check((host / "systemd" / "private").exists(), "launching needs the host's systemd user session")
    run = home / "run"
    (run / "systemd").mkdir(parents=True, exist_ok=True)
    os.chmod(run, 0o700)
    (run / "bus").symlink_to(host / "bus")
    (run / "systemd" / "private").symlink_to(host / "systemd" / "private")


class Events:
    """mirad's event stream, written to a file as it comes."""

    def __init__(self, mirad):
        self.path = mirad.home / "launch-events.txt"
        self.reader = subprocess.Popen(["curl", "-sN", "--unix-socket", str(mirad.sock), "http://mirad/v1/events"],
                                       stdout=self.path.open("wb"), stderr=subprocess.DEVNULL)
        time.sleep(0.3)

    def exits(self, game_id):
        text = self.path.read_text(errors="replace")
        return sum(1 for line in text.splitlines()
                   if '"state":"exited"' in line and f'"id":"{game_id}"' in line)

    def close(self):
        self.reader.terminate()
        self.reader.wait()


@scenario("launch", startup=True)
def launch(ctx, meta):
    """Starting a game and its exit reaching clients, for a native game, an Epic one (whose sign-in comes
    from legendary) and a GOG one (with comet beside it), five times each; then mirad left idle."""
    home = ctx.mirad.home
    games = [{"id": "bench-native", "name": "Bench Native", "source": "manual"},
             {"id": "epic-Bench", "name": "Bench Epic", "source": "epic", "ref": "Bench"},
             {"id": "gog-2000", "name": "Bench GOG", "source": "gog", "ref": "2000"}]
    add_games(home, games)
    env = fake_stores(home)
    link_systemd(home)
    start_with(ctx, env)
    events = Events(ctx.mirad)
    try:
        for game, kind in zip(games, ("native", "Epic", "GOG")):
            end_file = home / "store-games" / game["id"] / "run.sh.end"

            def play():
                before = events.exits(game["id"])
                status, body = ctx.client.request("POST", f"/v1/games/{game['id']}/launch")
                check(status == 200 and body.get("tracked"), f"launching {kind} answered {status}: {body}")
                started = time.perf_counter()
                deadline = time.time() + 30
                while events.exits(game["id"]) == before:
                    check(time.time() < deadline, f"the {kind} game's exit never reached clients")
                    time.sleep(0.002)
                seen = time.time()
                return started, seen - float(end_file.read_text())

            if ctx.mode == "instr":
                with ctx.measure(f"launch and exit, {kind}", repeats=5):
                    for _ in range(5):
                        play()
                continue
            starts, exits = [], []
            for _ in range(5):
                before = time.perf_counter()
                started, exit_delay = play()
                starts.append((started - before) * 1000)
                exits.append(exit_delay * 1000)
            ctx.results[f"start, {kind}"] = round(statistics.median(starts), 1)
            ctx.results[f"exit to exited, {kind}"] = round(statistics.median(exits), 1)
        if ctx.mode == "time":
            # Lanes started for the games' achievements end once idle; the threads below show they did.
            time.sleep(40)
    finally:
        events.close()


def kv(kind, name, value=b""):
    """Valve's binary KeyValues, as Steam writes its stats files: 0 an object, 1 text, 2 a 32-bit int."""
    key = bytes([kind]) + name.encode() + b"\0"
    if kind == 0:
        return key + value + b"\x08"
    if kind == 1:
        return key + value.encode() + b"\0"
    return key + struct.pack("<I", value)


def steam_stats(root, appid, account, count):
    """Steam's stats files for `count` achievements, a tenth hidden, every other one unlocked."""
    blocks, progress, percentages = b"", b"", []
    for stat in range((count + 31) // 32):
        bits, data, times = b"", 0, b""
        for bit in range(min(32, count - stat * 32)):
            n = stat * 32 + bit
            display = (kv(0, "name", kv(1, "english", f"Achievement {n}") + kv(1, "german", f"Erfolg {n}"))
                       + kv(0, "desc", kv(1, "english", f"Do thing number {n} in the game"))
                       + kv(2, "hidden", int(n % 10 == 9)) + kv(1, "icon", f"a{n}.jpg") + kv(1, "icon_gray", f"a{n}g.jpg"))
            bits += kv(0, str(bit), kv(1, "name", f"ACH_{n:03d}") + kv(0, "display", display))
            percentages.append({"name": f"ACH_{n:03d}", "percent": round(100 / (n + 1), 1)})
            if n % 2 == 0:
                data |= 1 << bit
                times += kv(2, str(bit), 1700000000 + n * 60)
        blocks += kv(0, str(stat + 1), kv(0, "bits", bits) + kv(1, "type", "ACHIEVEMENTS"))
        progress += kv(0, str(stat + 1), kv(2, "data", data) + kv(0, "AchievementTimes", times))
    stats = root / "appcache" / "stats"
    stats.mkdir(parents=True, exist_ok=True)
    (root / "steamapps").mkdir(exist_ok=True)
    (stats / f"UserGameStatsSchema_{appid}.bin").write_bytes(kv(0, appid, kv(0, "stats", blocks)) + b"\x08")
    (stats / f"UserGameStats_{account}_{appid}.bin").write_bytes(kv(0, "cache", kv(2, "crc", 7) + progress) + b"\x08")
    return percentages


def achievement_icons(folder, count):
    from PIL import Image  # only here: the other scenarios run without it
    folder.mkdir(parents=True, exist_ok=True)
    for n in range(count):
        for name, tint in ((f"a{n}.jpg", (40 + n % 200, 120, 200)), (f"a{n}g.jpg", (90, 90, 90))):
            Image.new("RGB", (256, 256), tint).save(folder / name, "JPEG", quality=85)


def steam_achievements(home, appid="4000", count=200):
    """A Steam game with `count` achievements, its icons and rarity, all on disk. Returns mirad's extra env."""
    account = 1000
    root = home / "steam-root"
    percentages = steam_stats(root, appid, account, count)
    endpoints = home / "endpoints"
    rarity = endpoints / "STEAM_API" / "ISteamUserStats" / "GetGlobalAchievementPercentagesForApp" / "v2"
    rarity.parent.mkdir(parents=True, exist_ok=True)
    rarity.write_text(json.dumps({"achievementpercentages": {"achievements": percentages}}))
    achievement_icons(endpoints / "STEAM_ICONS" / appid, count)
    add_settings(home, f'[steam]\nroot = "{root}"\nsteamid64 = "{76561197960265728 + account}"\n')
    return {"MIRA_STEAM_API_URL": f"file://{endpoints / 'STEAM_API'}",
            "MIRA_STEAM_ICONS_URL": f"file://{endpoints / 'STEAM_ICONS'}"}


@scenario("achievements", startup=True)
def achievements(ctx, meta):
    """A Steam game's 200 achievements read from its stats files, then served, with their icons; and an Epic
    game's 200 from legendary."""
    home = ctx.mirad.home
    add_games(home, [{"id": "bench-steam", "name": "Bench Steam", "source": "manual", "runner_ref": "steam:4000"},
                     {"id": "epic-Bench", "name": "Bench Epic", "source": "epic", "ref": "Bench"}])
    epic = {"completed": [{"name": f"e{n}", "display_name": f"Epic {n}", "description": "Win", "unlocked": True,
                           "unlock_date": "2024-05-01 12:00:00+00:00", "rarity": {"percent": 12.5}, "xp": 10,
                           "icon_link": f"https://cdn.example/e{n}.png"} for n in range(100)],
            "in_progress": [],
            "uninitiated": [{"name": f"e{n}", "display_name": f"Epic {n}", "description": "Later", "unlocked": False,
                             "rarity": {"percent": 50.0}, "xp": 5} for n in range(100, 200)],
            "hidden": []}
    env = fake_stores(home, epic) | steam_achievements(home)
    start_with(ctx, env)

    def refresh(game_id):
        state = ctx.client.wait_job(ctx.client.request("POST", f"/v1/games/{game_id}/achievements/refresh"))
        check(state.get("state") == "finished" and state["result"]["total"] == 200, f"{game_id}'s refresh: {state}")
    # The first refresh also fetches rarity; later ones parse the same files again.
    with ctx.measure("first refresh, Steam"):
        refresh("bench-steam")
    ctx.repeat("refresh, Steam", 5, lambda: refresh("bench-steam"))
    ctx.repeat("refresh, Epic", 5, lambda: refresh("epic-Bench"))

    def get():
        status, body = ctx.client.get("/v1/games/bench-steam/achievements")
        check(status == 200 and len(body["items"]) == 200 and body["unlocked"] == 100, "the 200 achievements")
    ctx.repeat("GET achievements, 200", 20, get)

    keys = [f"ACH_{n:03d}" for n in range(0, 40, 2)]

    def icons():
        for key in keys:
            status, data = ctx.client.request("GET", f"/v1/games/bench-steam/achievements/icon?key={key}&state=unlocked",
                                              raw=True)
            check(status == 200 and data[:3] == b"\xff\xd8\xff", f"{key}'s icon answered {status}")
    with ctx.measure("20 icons, first time"):
        icons()
    with ctx.measure("20 icons, cached"):
        icons()


def gui_pid(launcher, timeout=30):
    """mira-gui's own pid in the session it was started in. A child it forks is named mira-gui too until it
    execs, so the one whose parent isn't mira-gui."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        check(launcher.poll() is None, f"the GUI exited with {launcher.returncode}")
        parents = {}
        for stat in Path("/proc").glob("[0-9]*/stat"):
            try:
                fields = stat.read_text().rsplit(")", 1)
                name = fields[0].split("(", 1)[1]
                rest = fields[1].split()
                parent, session = int(rest[1]), int(rest[3])
            except (OSError, IndexError, ValueError):
                continue
            if name == "mira-gui" and session == launcher.pid:
                parents[int(stat.parent.name)] = parent
        own = [pid for pid, parent in parents.items() if parent not in parents]
        if own:
            return min(own)
        time.sleep(0.02)
    raise Failed("mira-gui never started")


def main_thread_ns(pid):
    """CPU time of a process's main thread, where Qt does its work, in nanoseconds."""
    return int(Path(f"/proc/{pid}/schedstat").read_text().split()[0])


class Display:
    """Keys sent to the GUI's window on its Xvfb display."""

    def __init__(self, pid):
        environ = Path(f"/proc/{pid}/environ").read_text().split("\0")
        gui_env = dict(line.split("=", 1) for line in environ if "=" in line)
        self.env = os.environ | {k: gui_env[k] for k in ("DISPLAY", "XAUTHORITY") if k in gui_env}
        windows = self.xdotool("search", "--onlyvisible", "--pid", str(pid)).split()
        check(windows, "the GUI shows no window to send keys to")
        # No window manager on Xvfb, so the window is given focus by hand.
        self.xdotool("windowfocus", windows[-1])
        time.sleep(0.2)

    def xdotool(self, *args):
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=10).stdout

    def key(self, keys):
        self.xdotool("key", keys)

    def snapshot(self):
        """The screen, small and grey, to tell whether a key changed what's shown."""
        return subprocess.run(["import", "-silent", "-window", "root", "-resize", "160x90!", "-depth", "8", "gray:-"],
                              env=self.env, capture_output=True, timeout=10).stdout


def changed(before, after):
    """The share of a snapshot's pixels that differ clearly from another's."""
    return sum(abs(a - b) > 16 for a, b in zip(before, after)) / max(len(before), 1)


def respond(pid, press, may_idle=False):
    """Wall time from the GUI's main thread starting on `press` until it first pauses (0.3 s), and its CPU
    until it rests (2 s), which counts work it put off, such as a page rebuilt after a restyle. Both in ms.
    The clock starts with the GUI's work, not the press, so starting xdotool isn't counted. None when
    `may_idle` and the key did nothing."""
    before, start = main_thread_ns(pid), time.perf_counter()
    press()
    last, last_poll, began, last_busy, first = before, start, None, None, None
    while time.perf_counter() < start + 30:
        time.sleep(0.005)
        now, used = time.perf_counter(), main_thread_ns(pid)
        if used - last > 500_000:  # busy over a tenth of the poll
            began = began or last_poll
            last_busy = now
        last, last_poll = used, now
        if last_busy is None:
            if may_idle and now - start >= 2:
                return None
            check(now - start < 2, "the key did nothing")
            continue
        if first is None and now - last_busy >= 0.3:
            first = last_busy
        if now - last_busy >= 2:
            return (first - began) * 1000, (used - before) / 1e6
    raise Failed("the GUI never came to rest after a key")


def hidden_mode(ctx, label, page_keys=None):
    """Ctrl+H in and out of hidden mode three times, on the library or the page `page_keys` opens."""
    def toggle(pid):
        display = Display(pid)
        if page_keys:
            shown = display.snapshot()
            wall, _ = respond(pid, lambda: display.key(page_keys))
            check(changed(shown, display.snapshot()) > 0.2, f"{page_keys} didn't open the page")
            ctx.results["open the page"] = round(wall, 1)
        walls, cpus = [], []
        for _ in range(6):
            wall, cpu = respond(pid, lambda: display.key("ctrl+h"))
            walls.append(wall)
            cpus.append(cpu)
        ctx.results["Ctrl+H"] = round(statistics.median(walls), 1)
        ctx.results["Ctrl+H, CPU until done"] = round(statistics.median(cpus), 1)
    open_gui(ctx, label, then=toggle)


def open_gui(ctx, label, args=(), then=None):  # label names its log
    """The real GUI, offscreen, on the fixture: time until it goes quiet (library and art loaded), then its
    CPU while idle, which catches redraw loops, and its memory and threads. `then(pid)` runs after."""
    # On Xvfb rather than Qt's offscreen platform, which the AppImage doesn't bundle.
    env = ctx.mirad.env() | {"QT_QPA_PLATFORM": "xcb"}
    # A build tree has it under frontend/; an AppImage beside mirad.
    folder = Path(ctx.mirad.binary).parent
    binary = next(p for p in (folder / "frontend" / "mira-gui", folder / "mira-gui") if p.exists())
    log = open(ctx.mirad.home / f"{label}.log", "w")
    # Xvfb picks its own free display: xvfb-run -a checks lock files first, so base and branch starting
    # together both got :99, the second server failed silently and both GUIs shared the first's.
    read_end, write_end = os.pipe()
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_end), "-screen", "0", "1920x1080x24", "-nolisten", "tcp"],
                            pass_fds=(write_end,), stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    os.close(write_end)
    with os.fdopen(read_end) as displayfd:
        display = displayfd.readline().strip()  # written once it accepts clients
    check(display.isdigit(), f"Xvfb exited with {xvfb.poll()} before giving a display")
    gui = subprocess.Popen([str(binary), *args], env=env | {"DISPLAY": f":{display}"}, stdout=log,
                           stderr=subprocess.STDOUT, start_new_session=True)
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
        check(idle <= 10, f"the GUI used {idle * 10} ms of CPU in 5 idle seconds (a redraw loop?)")
        if then:
            then(pid)
        check(gui.poll() is None, f"the GUI exited with {gui.returncode}")
        # After `then`, so what it did is counted, as for mirad's memory at a scenario's end.
        ctx.results.update({f"gui.{k}": v for k, v in proc_status(pid).items()})
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
        xvfb.terminate()
        xvfb.wait(10)
        log.close()


def zoom(ctx, label):
    """Ctrl+minus until the tiles are smallest, then Ctrl+plus until they're largest, twice. Each step
    fetches the covers on screen at the new size; memory at the end shows whether the sizes left behind
    are let go."""
    def sweep(pid):
        display = Display(pid)
        walls, cpus = [], []
        for _ in range(2):
            for key in ("ctrl+minus", "ctrl+plus"):
                for _ in range(20):  # the window's width sets how many sizes there are
                    shown = display.snapshot()
                    step = respond(pid, lambda: display.key(key), may_idle=True)
                    # At the end of the range a press does nothing, but work the GUI does anyway can
                    # still start the clock, so a step counts only when the tiles changed.
                    if step is None or changed(shown, display.snapshot()) < 0.01:
                        break
                    walls.append(step[0])
                    cpus.append(step[1])
        check(len(walls) >= 4, f"only {len(walls)} zoom steps did anything")
        ctx.results["zoom step"] = round(statistics.median(walls), 1)
        ctx.results["zoom step, CPU until done"] = round(statistics.median(cpus), 1)
    open_gui(ctx, label, then=sweep)


@scenario("gui", gui=True)
def gui_library(ctx, meta):
    """Opening Mira on the library."""
    open_gui(ctx, "GUI library")


@scenario("gui-big-screen", gui=True)
def gui_big_screen(ctx, meta):
    """Opening Mira straight into big screen."""
    # Its home page showcases the focused game after a pause that would look like quiet; with no
    # pause, quiet is after the showcase.
    status, _ = ctx.client.request("PATCH", "/v1/config", {"frontend": {"big_screen_trailer_delay_ms": 0}})
    check(status == 200, f"setting the showcase delay answered {status}")
    open_gui(ctx, "GUI big screen", ["--big-screen"])


@scenario("gui-hidden-mode", gui=True)
def gui_hidden_mode(ctx, meta):
    """Switching hidden mode on the library, which restyles every widget the GUI has built."""
    hidden_mode(ctx, "GUI hidden mode")


@scenario("gui-hidden-mode-tags", gui=True)
def gui_hidden_mode_tags(ctx, meta):
    """Switching hidden mode on the Tags page."""
    hidden_mode(ctx, "GUI hidden mode on Tags", page_keys="ctrl+shift+F9")


@scenario("gui-pages", gui=True)
def gui_pages(ctx, meta):
    """Opening Settings, Runners and Tags from the library, each until it has settled."""
    def visit(pid):
        display = Display(pid)
        # Tags first, from the library: its cards look much like Runners'.
        for name, keys, back in (("Tags", "ctrl+shift+F9", None), ("Settings", "ctrl+comma", "Escape"),
                                 ("Runners", "ctrl+shift+F10", None)):
            shown = display.snapshot()
            wall, cpu = respond(pid, lambda: display.key(keys))
            check(changed(shown, display.snapshot()) > 0.05, f"{keys} didn't open {name}")
            ctx.results[f"open {name}"] = round(wall, 1)
            ctx.results[f"open {name}, CPU until done"] = round(cpu, 1)
            if back:
                respond(pid, lambda: display.key(back))
    open_gui(ctx, "GUI pages", then=visit)


@scenario("gui-game-card", gui=True, startup=True)
def gui_game_card(ctx, meta):
    """A game card with 100 achievements opened, then its achievements page with their icons, then both
    closed, three times; then the same with its saves page and 10 backups. What the card took should go
    with it."""
    home = ctx.mirad.home
    add_games(home, [{"id": "bench-steam", "name": "Bench Steam", "source": "manual", "runner_ref": "steam:4000"}])
    start_with(ctx, fake_stores(home) | steam_achievements(home, count=100))
    state = ctx.client.wait_job(ctx.client.request("POST", "/v1/games/bench-steam/achievements/refresh"))
    check(state.get("state") == "finished", f"the achievements refresh ended {state}")
    saves = home / "store-games" / "bench-steam" / "saves"
    write_saves(saves, 50, 10 << 20)
    # A base without saves answers 404 and gets no saves rounds.
    status, _ = ctx.client.request("PATCH", "/v1/games/bench-steam/config", {"saves.extra_paths": [str(saves)]})
    has_saves = status < 300
    for _ in range(10 if has_saves else 0):
        (saves / "slot0.sav").write_bytes(os.urandom(1024))
        state = ctx.client.wait_job(ctx.client.request("POST", "/v1/games/bench-steam/saves/backup"))
        check(state.get("state") == "finished", f"a backup ended {state}")

    def cycle(pid):
        display = Display(pid)
        # Searched for, so its tile is the first; a click selects it and gives the grid focus. Places are
        # the 1600x1000 window's on the 1920x1080 screen.
        display.key("ctrl+f")
        time.sleep(1.5)
        display.xdotool("type", "--delay", "20", "Bench Steam")
        time.sleep(1.5)
        cards, card_cpus, closed = [], [], []
        # The facts line under the game's name: achievements, then saves beside them. The saves page's
        # light cards change about 4% of the small snapshot, the achievements' icons far more.
        lines = [("achievements", 570, 0.05)] + ([("saves", 760, 0.02)] if has_saves else [])
        pages = {name: ([], []) for name, _, _ in lines}
        for name, x, least in lines:
            for round_ in range(1, 4):
                # Again each round: a closed card leaves the grid without focus.
                display.xdotool("mousemove", "500", "270", "click", "1")
                time.sleep(1.5)
                library = display.snapshot()
                wall, cpu = respond(pid, lambda: display.key("alt+Return"))
                check(changed(library, display.snapshot()) > 0.05, f"Alt+Enter didn't open the game card (round {round_})")
                cards.append(wall)
                card_cpus.append(cpu)
                shown = display.snapshot()
                wall, cpu = respond(pid, lambda: display.xdotool("mousemove", str(x), "302", "click", "1"))
                check(changed(shown, display.snapshot()) > least, f"the {name} line didn't open its page (round {round_})")
                pages[name][0].append(wall)
                pages[name][1].append(cpu)
                for _ in range(2):  # back to the card, then the card closed
                    respond(pid, lambda: display.key("Escape"), may_idle=True)
                check(changed(library, display.snapshot()) < 0.02, f"Esc didn't close the card (round {round_})")
                closed.append(proc_status(pid)["rss_kb"])
        ctx.results["open the card"] = round(statistics.median(cards), 1)
        ctx.results["open the card, CPU until done"] = round(statistics.median(card_cpus), 1)
        for name, (walls, cpus) in pages.items():
            ctx.results[f"open {name}"] = round(statistics.median(walls), 1)
            ctx.results[f"open {name}, CPU until done"] = round(statistics.median(cpus), 1)
        # Memory after a close swings by up to 20 MB between rounds without growing, so the later rounds'
        # lowest is compared: a leak raises every round.
        check(min(closed[len(closed) // 2:]) - closed[0] <= 2048,
              f"each card opened and closed kept memory: {closed} KB after each close")
    open_gui(ctx, "GUI game card", then=cycle)


@scenario("gui-zoom", gui=True)
def gui_zoom(ctx, meta):
    """Zooming the library's tiles from smallest to largest and back."""
    zoom(ctx, "GUI zoom")


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
    parser.add_argument("--list-gui", action="store_true", help="print the GUI's scenarios and stop")
    args = parser.parse_args()
    if args.list or args.list_gui:
        print(" ".join(name for name, _, _, is_gui in SCENARIOS if is_gui == args.list_gui))
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
