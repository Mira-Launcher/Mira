#!/usr/bin/env python3
"""Builds the benchmark library: a HOME with a big library (150 games) of every kind Mira imports, across many emulators and stores, cover and hero art,
store catalogs, tags (a tenth of the games hidden) and play history, packed as a tarball each scenario unpacks fresh.

The same seed gives the same library, so main and a branch are measured on identical input.
Usage: fixture.py --mirad PATH --out fixture.tar [--games 150] [--titles 1200] [--seed 1]
"""

import argparse
import io
import json
import random
import shutil
import sqlite3
import sys
import tarfile
import time
from contextlib import closing
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).parent))
from mirabench import HOME, Mirad  # noqa: E402

WORDS = ("star dust iron hollow night river crown echo neon wild frost ember shadow last sky lost deep "
         "quiet storm ghost glass rune tide void solar lunar arcade pixel drift rogue knight tower dream "
         "machine signal forest ocean desert garden city empire legend origin rift").split()

# Emulator sources added for the fixture: id, ROM folder (its display name), extensions, share of the games.
EMULATORS = [("mgba", "mGBA", [".gba", ".gbc"], 0.05), ("mesen", "Mesen", [".nes", ".gb"], 0.04),
             ("snes9x", "Snes9x", [".sfc"], 0.03), ("duckstation", "DuckStation", [".cue"], 0.03),
             ("pcsx2", "PCSX2", [".chd"], 0.03), ("ppsspp", "PPSSPP", [".iso"], 0.03),
             ("dolphin", "Dolphin", [".rvz"], 0.03), ("eden", "Eden", [".nsp"], 0.03),
             ("azahar", "Azahar", [".3ds"], 0.02), ("melonds", "melonDS", [".nds"], 0.02),
             ("rmg", "RMG", [".z64"], 0.02), ("flycast", "Flycast", [".cdi"], 0.02),
             ("blastem", "BlastEm", [".md"], 0.02), ("stella", "Stella", [".a26"], 0.02)]
# Store catalogs' share of the owned-but-not-installed titles.
CATALOGS = [("epic", 0.3), ("gog", 0.2), ("steam", 0.3), ("amazon", 0.1), ("itch", 0.1)]
TAGS = ["RPG", "Action", "Puzzle", "Platformer", "Strategy", "Co-op", "Indie", "Racing", "Favorites"]
SETTINGS = """auto_setup = false

[metadata]
enabled = false

[launchers]
auto_import = false
umu_lookup = false

[updates]
check = false
"""
FRONTEND = """onboarded = true
window_width = 1600
window_height = 1000

# The Tags and Runners pages have no default keys, and the GUI scenarios reach them by keyboard.
[shortcuts]
open_tags = "Ctrl+Shift+F9"
open_runners = "Ctrl+Shift+F10"
"""


def title(rng, used):
    while True:
        name = " ".join(w.capitalize() for w in rng.sample(WORDS, rng.choice((1, 2, 2, 3))))
        if rng.random() < 0.2:
            name += f" {rng.randint(2, 4)}"
        if name not in used:
            used.add(name)
            return name


def art(rng, size, quality=85):
    """A JPEG of a real cover's weight: gradient, shapes and grain, about 50-120 KB at 600x900."""
    w, h = size
    top, bottom = [tuple(rng.randint(0, 255) for _ in range(3)) for _ in range(2)]
    image = Image.new("RGB", size)
    draw = ImageDraw.Draw(image)
    for y in range(h):
        t = y / h
        draw.line([(0, y), (w, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    for _ in range(12):
        x, y = rng.randint(0, w), rng.randint(0, h)
        r = rng.randint(w // 20, w // 4)
        draw.ellipse([x - r, y - r, x + r, y + r], fill=tuple(rng.randint(0, 255) for _ in range(3)))
    image = image.filter(ImageFilter.GaussianBlur(2))
    grain = Image.effect_noise(size, 24).convert("RGB")
    image = Image.blend(image, grain, 0.12)
    out = io.BytesIO()
    image.save(out, "JPEG", quality=quality)
    return out.getvalue()


def make_files(home, rng, games):
    root = home / "Mira" / "Games"
    used = set()
    roms = [int(games * share) for *_, share in EMULATORS]
    native = int(games * 0.5)
    windows = games - native - sum(roms)
    elf = Path(shutil.which("true")).read_bytes()
    for _ in range(native):
        name = title(rng, used)
        folder = root / name
        (folder / "data" / "levels").mkdir(parents=True)
        exe = folder / (name.replace(" ", "") + ".x86_64")
        exe.write_bytes(elf)
        exe.chmod(0o755)
        for i in range(rng.randint(3, 12)):
            (folder / "data" / f"pack{i}.dat").write_bytes(rng.randbytes(rng.randint(1_000, 40_000)))
        (folder / "data" / "levels" / "1.lvl").write_bytes(rng.randbytes(2_000))
    for _ in range(windows):
        name = title(rng, used)
        folder = root / name
        folder.mkdir(parents=True)
        (folder / (name.replace(" ", "") + ".exe")).write_bytes(b"MZ" + rng.randbytes(60_000))
        (folder / "engine.dll").write_bytes(b"MZ" + rng.randbytes(20_000))
    for (_, rom_dir, exts, _share), count in zip(EMULATORS, roms):
        folder = home / "Mira" / "Roms" / rom_dir
        folder.mkdir(parents=True)
        for _ in range(count):
            name = title(rng, used) + " (USA)"
            ext = rng.choice(exts)
            if ext == ".cue":
                (folder / f"{name}.bin").write_bytes(rng.randbytes(50_000))
                (folder / f"{name}.cue").write_text(f'FILE "{name}.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
            else:
                (folder / f"{name}{ext}").write_bytes(rng.randbytes(rng.randint(30_000, 200_000)))


def seed_sources_and_catalogs(config_dir, rng, titles):
    with closing(sqlite3.connect(config_dir / "mira.db")) as db, db:
        for position, (emu, *_rest) in enumerate(EMULATORS, start=20):
            db.execute("INSERT INTO sources(id, added, enabled, in_sidebar, position, imported_at) VALUES(?, 1, 1, 1, ?, 0) "
                       "ON CONFLICT(id) DO UPDATE SET added = 1, enabled = 1, in_sidebar = 1", (emu, position))
        for position, (store, _share) in enumerate(CATALOGS, start=2):
            db.execute("INSERT INTO sources(id, added, enabled, in_sidebar, position, imported_at) VALUES(?, 1, 1, 1, ?, 0) "
                       "ON CONFLICT(id) DO UPDATE SET added = 1, enabled = 1, in_sidebar = 1", (store, position))
    used = set()
    now = int(time.time())
    with closing(sqlite3.connect(config_dir / "cache.db")) as db, db:
        for source, share in CATALOGS:
            for position in range(int(titles * share)):
                db.execute("INSERT INTO catalog(source, position, ref, title, play_seconds, owned) VALUES(?, ?, ?, ?, ?, 1)",
                           (source, position, f"{source}{position:05d}", title(rng, used), rng.choice((0, 0, 0, 3600, 72000))))
            db.execute("INSERT INTO catalog_checked(source, checked_at) VALUES(?, ?) "
                       "ON CONFLICT(source) DO UPDATE SET checked_at = excluded.checked_at", (source, now))


def seed_tags_and_history(config_dir, rng, ids, titles):
    """Returns the hidden games' and store titles' counts."""
    now = int(time.time())
    hidden = set(rng.sample(ids, len(ids) // 10))
    with closing(sqlite3.connect(config_dir / "mira.db")) as db, db:
        for game_id in ids:
            tags = rng.sample(TAGS, rng.randint(0, 3)) + (["hidden"] if game_id in hidden else [])
            for position, tag in enumerate(tags):
                db.execute("INSERT OR IGNORE INTO game_tags(game_id, position, tag) VALUES(?, ?, ?)", (game_id, position, tag))
        for game_id in rng.sample(ids, len(ids) // 4):
            played = now - rng.randint(60, 90 * 86400)
            db.execute("UPDATE games SET last_played_at = ?, last_session_at = ?, play_seconds = ? WHERE id = ?",
                       (played, played, rng.randint(300, 400_000), game_id))
        # Hidden store titles, where the base's mirad keeps them.
        hidden_titles = 0
        if db.execute("SELECT 1 FROM sqlite_master WHERE name = 'hidden_titles'").fetchone():
            for source, share in CATALOGS:
                for position in range(0, int(titles * share), 20):
                    db.execute("INSERT INTO hidden_titles(id) VALUES(?)", (f"{source}-{source}{position:05d}",))
                    hidden_titles += 1
    return len(hidden), hidden_titles


def seed_steam_tags(config_dir, rng, ids, count=400, per_game=20):
    """Steam's tags on every game, as a metadata fetch stores them: `per_game` each out of `count`, a few on
    most games and most on only a few, as on Steam. Returns how many different tags the games have."""
    names = sorted({f"{a.title()} {b.title()}" for a in WORDS for b in WORDS if a != b})
    names = rng.sample(names, count)
    weights = [1 / (rank + 1) for rank in range(count)]
    used = set()
    with closing(sqlite3.connect(config_dir / "cache.db")) as db, db:
        for game_id in ids:
            picked = set()
            while len(picked) < per_game:
                picked.add(rng.choices(range(count), weights)[0])
            tags = json.dumps({"appid": "1", "tags": [names[i] for i in sorted(picked)]})
            used |= picked
            db.execute("INSERT INTO metadata(id, info, updated_at) VALUES(?, json_object('steam_tags', json(?)), 1) "
                       "ON CONFLICT(id) DO UPDATE SET info = json_set(info, '$.steam_tags', json(?))",
                       (game_id, tags, tags))
    return len(used)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mirad", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--games", type=int, default=150)
    parser.add_argument("--titles", type=int, default=1200)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    rng = random.Random(args.seed)

    home = HOME
    shutil.rmtree(home, ignore_errors=True)
    config_dir = home / ".config" / "mira"
    config_dir.mkdir(parents=True)
    (config_dir / "settings.toml").write_text(SETTINGS)
    (config_dir / "frontend.toml").write_text(FRONTEND)
    make_files(home, rng, args.games)

    # First start: mirad makes its databases and imports the game folders.
    mirad = Mirad(args.mirad, home).start()
    mirad.wait_log("startup scan:")
    mirad.stop()

    # Emulator sources and store lists, then a second start imports the ROMs.
    seed_sources_and_catalogs(config_dir, rng, args.titles)
    mirad = Mirad(args.mirad, home).start()
    mirad.wait_log("startup scan:")
    client = mirad.client()
    _, games = client.get("/v1/games")
    games = games if isinstance(games, list) else games["games"]
    ids = sorted(g["id"] for g in games)
    sources = {}
    for game in games:
        sources[game["source"]] = sources.get(game["source"], 0) + 1
    print(f"{len(ids)} games imported: {sources}", file=sys.stderr)
    for i, game_id in enumerate(ids):
        cover = art(rng, (600, 900))
        state = client.wait_job(client.request("PUT", f"/v1/games/{game_id}/artwork?type=cover", cover,
                                               headers={"Content-Type": "image/jpeg"}))
        if state.get("state") != "finished":
            raise SystemExit(f"cover upload for {game_id} failed: {state}")
        if i % 4 == 0:
            hero = art(rng, (1920, 620), quality=80)
            client.wait_job(client.request("PUT", f"/v1/games/{game_id}/artwork?type=hero", hero,
                                           headers={"Content-Type": "image/jpeg"}))
    mirad.stop()

    hidden, hidden_titles = seed_tags_and_history(config_dir, rng, ids, args.titles)
    steam_tags = seed_steam_tags(config_dir, rng, ids)
    for leftover in ("run", "tmp", "mirad.log"):
        path = home / leftover
        shutil.rmtree(path) if path.is_dir() else path.unlink(missing_ok=True)
    with tarfile.open(args.out, "w") as tar:
        tar.add(home, arcname=".")
    shutil.rmtree(home)
    titles = sum(int(args.titles * share) for _, share in CATALOGS)
    Path(args.out).with_suffix(".json").write_text(json.dumps({"games": len(ids), "titles": titles, "sources": sources,
                                                                 "hidden": hidden, "hidden_titles": hidden_titles,
                                                                 "steam_tags": steam_tags}))
    print(f"fixture: {len(ids)} games -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
