# API reference

REST over HTTP/1.1 on a Unix socket, never TCP. The default socket is `$XDG_RUNTIME_DIR/mira/mirad.sock`; the `socket_path` setting or `mirad --socket <path>` changes it, and `$MIRA_SOCKET` points the GUI and CLI elsewhere. Routes are registered in `Server::RegisterRoutes` in `src/api/Server.cpp`, and the code wins if this file disagrees.

Bodies are JSON. Errors share one envelope:

```json
{ "error": { "code": "invalid_setting", "message": "scan.debounce_ms: must be between 0 and 600000" } }
```

`code` is stable and meant for code; `message` is meant for people and says what went wrong; it is never empty (a failure with nothing to say gets its code in words). Two optional fields say what to do about it:

- `hint`: one sentence for the user, worded for any client (no CLI commands, no GUI paths).
- `fix`: where the fix is, for a client to turn into a button or a command. `{"kind": "setting", "target": "<dotted key>"}`, `{"kind": "runners", "target": ""}` (install a runner) or `"target": "winetricks"`, `{"kind": "source", "target": "<source id>", "step": "setup" | "login" | "install"}`, `{"kind": "game", "target": "<game id>", "step": "exe" | "data_dir" | "log" | "install"}` (`install`: run its installer with the window shown), or `{"kind": "packages", "target": "<feature>"}` (install what `GET /v1/system/packages?for=<feature>` lists).

```json
{ "error": { "code": "no_steamgriddb_key", "message": "searching SteamGridDB needs an API key",
             "hint": "Add a free SteamGridDB API key. Steam games don't need one.",
             "fix": { "kind": "setting", "target": "steamgriddb.api_key" } } }
```

Every `*.failed` event (and `game.artwork_candidates_ready` or `game.artwork_thumbs_ready` with an error) carries the message as `error`, plus its `code` and, when they apply, the same `hint` and `fix`. `job.failed` nests them under `error` instead.

```sh
curl --unix-socket "$XDG_RUNTIME_DIR/mira/mirad.sock" http://localhost/v1/health
```

Long-running work (scans, downloads, installs, winetricks) is a [job](#jobs): it answers `202` straight away, and a client that missed its events can ask `GET /v1/jobs/{id}`. Progress that a job has no steps for, such as an install's progress bar, arrives on the [event stream](#events).

## Jobs

Scans, imports, moving games, removing games, removing a source, bulk metadata refreshes, installs, store and tool setup, Humble downloads, runner downloads, winetricks verbs and choosing artwork are jobs. Their own `*.started`, `*.finished` and `*.failed` events are published as well, for clients that follow one thing. The request checks its input as usual (a bad body is still `400`), then answers `202 {"status": "running", "job": "<id>"}` and does the work in the background. Pass `?job=<id>` (letters, digits, `-`, `_`, up to 64) to name the job yourself, so you can listen for it before the reply arrives.

Events: `job.started {id, kind, target, label}`, `job.progress {id, done, total, message}` where the work has steps, then `job.finished {id, kind, target, result}` or `job.failed {id, kind, target, error}`. `result` is what the endpoint describes as its reply; `error` is the usual `{code, message, hint?, fix?}`, or `internal_error` for a bug in mirad. `kind` is `scan`, `import`, `relocate`, `delete`, `remove_source`, `metadata`, `install`, `update`, `setup`, `download`, `runner`, `tricks`, `artwork`, `achievements` or `saves`; `target` is the source, game, store, tool or runner it's about, or empty. Installs, runner downloads and tool setups run a few at a time, and winetricks verbs one at a time.

### `GET /v1/jobs/{id}`
`{id, kind, target, label, state, progress?, result?, error?}` with `state` `running`, `finished` or `failed`. The last 100 jobs are kept, never dropping one still running; an older one is `404 job_not_found`.

### `POST /v1/jobs/{id}/cancel`
Stops a running job: the programs it runs (a store tool, an installer and the Wine processes in its prefix, a download) are killed, and it ends as `job.failed` with code `cancelled`. Its own failure event (`library.install.failed`, `game.install.failed`, `launcher.install.failed`, `runners.download.failed`, `<tool>.setup.failed`) carries `code: "cancelled"` too. Store tools keep what they downloaded, so installing again resumes. `{"status": "cancelling"}`; `404 job_not_found`, or `409 not_running` once it has ended.

## Health

### `GET /v1/health`
`{"status": "ok", "api": 2}`. `api` is an integer bumped on a breaking change; the GUI and CLI compare it with their own and report a daemon that doesn't match.

## Settings

Stored in `settings.toml`, which holds only the settings that were set: every other one reads its default, so a changed default reaches everyone who never picked a value. Every key is declared once in `src/config/Schema.cpp`.

### `GET /v1/config[?reveal=1]`
Every setting at its current value, plus `secrets_set` and the `frontend` table. A secret (`is_secret` in the schema, such as `steamgriddb.api_key`) reads as `""`, and `secrets_set` lists the secrets that have a value, so a dump of the config never carries them; `?reveal=1` returns them as stored (`mira config get` uses it). Writing one is a plain `PATCH`, and a reset clears it. The `frontend` table is what the backend stores without interpreting: `frontend.toml`, with the window state the GUI sets as it's used (sizes, sort, last filter, `onboarded`, `source_imported_at`) merged in from `mira.db`.

### `GET /v1/config/schema`
Every setting in display order:

```json
[{ "key": "scan.debounce_ms", "label": "Scan delay (ms)", "type": "an integer",
   "default": 3000, "scope": "global", "category": "Scanning", "group": 0,
   "group_label": "Scanning",
   "doc": "How long a new folder must stay unchanged before Mira scans it.",
   "minimum": 0, "maximum": 600000 }]
```

`category` is the settings section. Within it, settings sharing a `group` number belong together under the title `group_label`. Labels and group titles are in sentence case. `scope` is `per_game` when a game can override the setting and `game_only` when it only exists per game. Optional fields, present only when they apply: `game_doc` (help text for a game's own settings, used instead of `doc`), `one_of` (enum values), `minimum`/`maximum`, `is_secret` (mask the value), `is_runner_ref` (offer a runner picker), `runner_kinds` (the runner kinds that picker lists, such as `["native"]`, with no `auto`; without it, every runner and `auto`), `link` (a web page where the user gets the value, e.g. an API key page), `keywords` (extra search terms such as abbreviations, space-separated), `group_collapsed` (the group holds rarely changed values, best shown folded), `group_resettable` (the group's values make sense to put back to their defaults together, so a screen can offer one reset for the group), `path` (`"folder"` or `"file"` when the value, or each item of an array, is a path on this computer, so a screen can offer a picker) and `source` (the source the setting belongs to, e.g. `"steam"` or `"ubisoft"`, so a screen can gather one source's settings together).

### `PATCH /v1/config`
Sets any subset of settings, nested like `GET /v1/config`, plus an optional `frontend` key, merged into `frontend.toml` as a JSON merge patch (a `null` value deletes that key). The whole patch is validated first; one bad value means nothing is applied.

### `POST /v1/config/reset[?key=<dotted.key>]`
Resets one key, or everything when `key` is left out.

## Games

Stored in the library database, `mira.db`. A game's `id` is a readable slug such as `celeste`, or `celeste-2` on a clash.

### `GET /v1/games[?status=][&tag=][&include_hidden=true][&include_off=true]`
Lists games, optionally filtered by `status` (`setting_up`, `ready`, `broken`, `missing`, `needs_install`) and by tag, matched ignoring case. Games tagged `hidden` are left out unless `tag` is given, so `?tag=hidden` lists only those, or `include_hidden=true` is, which lists them alongside the rest. Games from a source that's switched off are left out too, unless `include_off=true` is given.

### `GET /v1/games/{id}`

```json
{
  "id": "celeste", "install_path": "/home/x/Games/Celeste", "name": "Celeste",
  "status": "ready", "confidence": 0.9, "reviewed": false, "platform": "native",
  "source": "scan", "exe_path": "Celeste", "args": "", "working_dir": "", "runner_ref": "",
  "data_dir": "", "runner_config": {}, "overrides": {}, "last_error": "",
  "created_at": 0, "updated_at": 0, "last_played_at": null, "play_seconds": 0,
  "env": {}, "candidates": [], "tags": [], "running": false, "needs_check": false,
  "art": {"cover": "18f3a2c07d4e1b00-2c41"}
}
```

- `running` is whether Mira is tracking a process for the game right now. Every game record the API returns or an event carries has it.
- `tools` lists the game's [tool options](#launch-options) open right now, `[{id, name}]`, empty when none is. Opening or closing one publishes `game.updated`.

- `art` lists the art slots Mira has an image cached for, each with a version: `{"cover": "18f3a…-2c41", "hero": "…"}`. A slot left out has no image, so `GET /v1/games/{id}/artwork` for it would 404. The version changes whenever the slot's image does, so a client can keep its copy until then. Every game record has it, and so do `game.metadata_ready`, `game.metadata_failed` and `game.artwork_selected`.

- `store_url` is the game's page on the store it's from, when there is one: Steam's from its appid, itch's and GOG's from the store's last listing (`GET /v1/library`). Left out otherwise.

- `confidence` is how sure the detector was of its pick of executable; it stays 0 for games an importer added, which have no `candidates`. `reviewed` turns true once someone changes or confirms the game.
- `needs_check` is true for a game Mira picked the executable for itself (it has `candidates`), with `confidence` under `detect.low_confidence_threshold`, that isn't `reviewed` yet. It is how a client marks games to look at.
- `candidates` lists every executable the detector considered.
- `runner_config` belongs to the runner named by `runner_ref`.
- `default_runner`, on this call only, is the `kind:name` the game would run with if `runner_ref` were empty (its source's runner, else `default_runner.*`, with `auto` resolved to a kind), so an editor knows whose options to show for "Default runner".
- `data_dir` is the game's prefix.
- `installer_dir` is the folder the game's installer was in, once the game was installed somewhere else (usually its prefix). A scan treats that folder as this game's and doesn't add it again.
- `library_link` is the game's link in a library folder when it's [sorted by tag](#folders-by-tag) but installed inside its prefix; empty otherwise. `folder_tag`, `sort_root`, `folder_tags` and `folder` are described there too.
- `source` says where the game came from: `scan`, `manual`, `steam`, `lutris`, `epic`, `gog`, `itch`, `amazon`, a launcher id, and so on. That source owns the fields it writes on a re-import.

### `PATCH /v1/games/{id}`
Changes any of `name`, `exe_path`, `args` (one command line: arguments are split on spaces, and quotes keep one together, as in `--save "C:\My Games"`), `working_dir`, `runner_ref`, `data_dir`, `platform` (`windows` or `native`; anything else is `400 invalid_body`; a game turned `windows` without a `data_dir` gets one under `prefix_root` (or, on another disk, that disk's [prefixes folder](#library-folders)), and a ready one becomes `setting_up` while a `provision` [job](#jobs) sets up its prefix, then `ready` or `broken` with a `game.updated`; a game turned `native` keeps its `data_dir`, so turning it back reuses the prefix), `runner_config` (merged), `env` (merged, `null` removes a key), `tags` (replaced), `launch` (replaced, see [Launch options](#launch-options)) and `folder_tag` (in a library folder sorted by tag, a change to either can move the game, see [Folders by tag](#folders-by-tag)). An `exe_path` given relative but outside the game's folder (`../Applications/Eden.AppImage`) is stored absolute, here and in `POST /v1/games/manual`, so it survives a move. Any change marks the game `reviewed`; `{"reviewed": true}` confirms a game without changing anything else. Overrides go through `/config` below. Publishes `game.updated`.

### `PATCH /v1/games`
Changes many games in one request, for a multi-select:

```json
{ "ids": ["celeste", "hades"], "add_tags": ["hidden"], "remove_tags": ["favorite"],
  "config": { "desktop_entries.enabled": false } }
```

`ids` is required; the rest are optional. `"folder_tag": "RPG"` picks that tag as each game's folder (adding it if missing; `""` goes back to `tags.folders`' order, see [Folders by tag](#folders-by-tag)). `config` takes the same overrides as `PATCH /v1/games/{id}/config`, and a bad key rejects the whole batch. Unknown ids are skipped. Returns `{"games": [...]}` with only the games that changed, and publishes one `games.updated` event for them all. Unlike `PATCH /v1/games/{id}`, it doesn't mark games `reviewed`.

### `POST /v1/games/manual`
Adds a game from any path:

```json
{ "install_path": "/abs/path/to/folder", "exe_path": "relative/Installer.exe",
  "name": "My Game", "platform": "windows", "is_installer": true }
```

`install_path` and `exe_path` (relative to `install_path`) are required. `name` defaults to the cleaned folder name, or the file's own name for an AppImage, and `platform` to `windows` for `.exe`, else `native`; a `platform` other than those two is `400 invalid_body`. `is_installer` stores the game `needs_install`. `app: true` tags it `app`. A Windows game is returned `setting_up` while a `provision` [job](#jobs) sets up its prefix, then becomes `ready` or `broken` with a `game.updated`. Adding the same `install_path` and `exe_path` again updates that game, and so does another program in its folder. An AppImage is a game of its own, so each AppImage in one folder is a separate game. Returns the game and publishes `game.added` or `game.updated`.

### `DELETE /v1/games/{id}[?delete_files=true][&delete_prefix=true][&delete_metadata=true][&purge=true][&delete_saves=true]`
Removes the game from the library. Nothing on disk is touched unless asked:

- `delete_files` removes `install_path`.
- `delete_prefix` removes `data_dir`.
- `delete_metadata` forgets the game entirely: its fetched info, the art the user picked and its kept record (below).
- `purge` does all three.
- `delete_saves` removes its [save backups](#saves), which are the user's own data: `purge` keeps them. Only the game's folder inside the backup folder is deleted. Ignored when `delete_files` hands a Steam game to Steam's uninstall.

A removed game keeps a small record of what the user set on it: its name, exe, arguments, runner, prefix path, folder tag, overrides, environment, launch options and tags. A game added again under its id (a store game installed again) or found again where it was (`install_path`, and the exe for an AppImage or desktop entry) takes the same id back with that record, and its play history. A kept prefix that's still there becomes its prefix again, saves and all. A store game's fetched info stays for its store's Not installed list; any other game keeps only the art the user picked.

Files are only deleted when they resolve inside a library root, a store's install root (`epic.`, `gog.`, `itch.`, `amazon.install_root`) or the game's own prefix, and prefixes inside `prefix_root` or another disk's [prefixes folder](#library-folders). A `desktop-entry` game's files belong to another app and are never deleted, though a prefix Mira made for it can be. For Epic, Amazon and itch.io games, `delete_files` uninstalls through `legendary`, `nile` or butler so the store's records stay correct. A game run from an AppImage loses just the AppImage; for any other game, a folder that also holds another game or a library root isn't deleted (`shared_folder`). A program the game only runs (see relocate below) is never deleted. Publishes `game.removed`.

For a Steam game, `delete_files` opens Steam's own uninstall instead (`steam://uninstall/<appid>`), which asks first and takes its compatdata too; the job's result is `{"handed_to": "steam"}`. The game stays until Steam has uninstalled it, then leaves the library as below; if the uninstall is cancelled, nothing changes. `delete_prefix` is ignored for Steam games.

With `delete_files`, `delete_prefix` or `purge` it is a [job](#jobs) (deleting can take minutes) whose result is `{}`, failing with the error that kept the game; otherwise it answers `{}` straight away.

A store import (`POST /v1/steam/scan`, `POST /v1/stores/{id}/import`) also removes a game the store no longer lists as installed whose files are gone while the folder they were in is still there: it was uninstalled in the store's own client. It keeps its record and publishes `games.removed`. A game whose folder is gone too (a disk that isn't mounted) is marked `missing` instead.

### `POST /v1/games/delete`
The same for many games: `{"ids": [...], "delete_files"?, "delete_prefix"?, "delete_metadata"?, "purge"?, "delete_saves"?}`, flags as above. A folder shared only by games removed together is deleted; one another game still uses isn't (`shared_folder`). A game whose files or prefix can't be deleted stays in the library. Unknown ids are skipped. Steam games with `delete_files` go to Steam's own uninstall, one dialog each. A [job](#jobs) whose result is `{"removed": [ids], "handed_to_steam": [ids], "failed": [{"id", "error": {...}}]}`, with each error shaped like the error envelope, and publishes one `games.removed` event with the removed `ids`.

### `POST /v1/games/removal`
What removing each game can delete, to show before asking: `{"ids": [...]}` answers a list, in order and without unknown ids, of `{id, name, kind, tool, files, files_in_prefix, folder_is_prefix, prefix, prefix_shared_with, saves}`:

- `kind`: `steam` (Steam's own uninstall takes it), `store` (its store's tool, or Mira for GOG, deletes `files`), `folder` (Mira deletes `files`) or `external` (its files aren't Mira's: a desktop entry, Lutris, an emulator's or launcher's game, or one outside every Mira folder; only `delete_prefix` does anything).
- `tool`: who deletes a store game's files (`Steam`, `Legendary`, `Nile`, `butler`, `Office`), `""` for Mira.
- `files`: the folder or AppImage `delete_files` removes, `""` for `external`. `files_in_prefix` is the same as the game sees it (`C:\Games\X`) when it's installed inside its prefix, else `""`. `folder_is_prefix` is true when the folder is the prefix itself, so `delete_files` removes the prefix.
- `prefix`: the prefix Mira made that `delete_prefix` removes, `""` when there's none (Steam games, a prefix Mira didn't make). `prefix_shared_with` names another game using it, which keeps it (`shared_prefix`).
- `saves`: the folder of its save backups that `delete_saves` removes, null when it has none.

### `POST /v1/games/removal/sizes`
The sizes behind those plans, as a [job](#jobs) since big folders take a while: `{"ids": [...]}`, result `{"<id>": {"files": bytes, "prefix": bytes, "saves": bytes}}`. For a game installed inside its prefix, `prefix` is the rest of it. `saves` counts a file shared between backups once.

### `GET /v1/games/{id}/config`
Every setting as it resolves for this game, with the layer it came from:

```json
{ "launch.gamemode": { "value": true, "layer": "game", "overridable": true },
  "library_roots": { "value": ["~/Mira/Games", "~/Mira/Applications"], "layer": "default", "overridable": false } }
```

### `PATCH /v1/games/{id}/config`
Sets or removes (`null`) this game's overrides as a flat `{"dotted.key": value}` body. A value equal to what the setting is without an override (from settings, or its default) removes the override too, so the game keeps following that setting. Only `per_game` keys are accepted, and nothing is applied if any key fails.

### `POST /v1/games/{id}/launch`
Resolves `runner_ref` (or the platform's `default_runner.*`) and starts the game. A Microsoft 365 app takes an optional body `{"files": ["/absolute/path", ...]}` and opens those documents, also while it's already running (`400 files_unsupported` for anything else); its desktop entry declares the file types it opens and passes them on. `409 needs_install` or `409 not_ready` when it can't run, `409 game_busy` while it's being moved or its files deleted (a move or delete likewise waits for no launch and fails with `game_busy` or `game_running`). While its saves are being backed up or synced, as just after it exits, the launch waits for that to finish first (`game.saves_activity` says it's going on); mirad stopping meanwhile answers `503 shutting_down`. A Windows game left `broken` by a missing runner is provisioned again first.

- `command_wrappers` are prepended in order, first outermost. Each entry is split on spaces, so `"gamescope -W 1920 -H 1080"` is one entry. A wrapper missing from `PATH` fails with `400 wrapper_not_found`.
- `launch.env` applies under the runner's environment; the game's own `env` wins over both.
- `launch.pre_script` runs through `sh -c` before the game and blocks the request. A non-zero exit fails with `409 pre_launch_failed` and the script's output.
- `launch.post_script` runs after the game exits, however it exits. Its result is only logged.
- `launch.gamemode` registers the game with GameMode for its lifetime.
- An Epic game gets Epic's sign-in arguments from `legendary launch --json` (online, else `--offline`, up to 10 s each), so it can reach Epic's services and unlock achievements. When legendary gives none, the game starts without them and the daemon log says why. The exchange code in them is masked in the game's log and never appears in an event or reply, and legendary's ownership token file is deleted when the game exits.
- A GOG or Epic game's [cloud saves](#cloud-saves) are synced first, while `saves.cloud` is on. Saves changed both here and in the cloud refuse the launch with `409 saves_conflict` (fix: the game's saves) and publish a persistent `notification`; the body's `"saves": "keep_local"` (upload this computer's), `"use_cloud"` (download the cloud's, after a backup) or `"skip"` (no sync) settles it for this launch, anything else is `400 invalid_body`. A cloud that can't be reached, or takes over `saves.cloud_timeout_s` (default 20) to answer, lets the game start with the saves here; a download already under way is let finish.
- A game whose saves Mira keeps and that has no backup yet gets one first (reason `first`), so even its first session can be undone.
- While `gog.comet` is on for a GOG game, [comet](https://github.com/imLinguin/comet) starts before it, standing in for GOG Galaxy so the game's achievements unlock. One comet serves every running GOG game and is stopped once none is running; its output goes to `logs/comet.log`. It signs in with gogdl's sign-in, which it reads through a link, never from its command line. Signed out of GOG or without comet, the game starts without it and the daemon log says why.

The game runs under `mira-run`, which owns the scripts, the game's output log and the session record, so a session survives `mirad` restarting. If `mira-run` is missing, the game is started directly without a session record or log.

The reply's `tracked` says whether `game.state` events will follow. It is false for a Steam game under `steam.launch_mode: "steam"` (the default), which is started through `steam -silent -applaunch <appid>` (`-silent`, on unless `steam.launch_silent` is off, keeps a Steam that wasn't running from opening its window). With `steam.track_process` on, Mira still finds the game's process by its `SteamAppId`/`SteamGameId` and records playtime, but gets no exit code. `steam.launch_mode: "direct"` runs the game through Steam's Proton build and prefix with full tracking, but needs `exe_path` set by hand.

Launching a store launcher game (Battle.net, Ubisoft, EA) asks the launcher to start it and tracks the game's own processes.

#### Launch options
A game's `launch` holds other ways to start it and programs that run beside it, such as a mod manager or a deck tracker:

```json
{ "default": "olympus", "only": false,
  "options": [{ "id": "olympus", "name": "Olympus", "mode": "play", "exe": "/home/me/Olympus/olympus", "args": "", "native": true }] }
```

- `mode` `play` starts the option instead of the game's own program, however the game's source would start it, tracked as the game (its time counts as playtime). `alongside` starts with every tracked launch of the game and is stopped when the game exits. `tool` is opened by itself in a systemd scope of its own: not a play session (no playtime, no `game.state`), but listed in the game's `tools` while anything in that scope runs, so a launcher that starts the real program and exits still counts. Tools opened before mirad restarted aren't listed.
- `exe` is absolute or relative to `install_path`. `{game_dir}`, `{game_exe}` and `{prefix}` in `exe` or `args` are filled in, as `Z:\...` Wine paths in the args of an option run in the prefix.
- A Windows game's option runs with the game's runner and in its prefix (Steam's own Proton and prefix for a Steam game), unless `native` is set. Any other game's runs as a Linux program.
- `sign_in` (optional, `play` only): whether the option starts with the store's sign-in, as the game's own program does: Epic's sign-in arguments, which hold a single-use credential. Unset, it does when `exe` is inside `install_path` (another program of the game, such as its DirectX 12 build), and doesn't for a program elsewhere, such as a mod manager, which can't use the code before it expires. GOG's comet runs beside every play option either way, since it hands the program nothing.
- `default` is the `play` option Play starts with no option named; empty for the game's own program. `only` asks a client to offer nothing else. `id`s are made from the name when missing; `game` is reserved for the game's own program.

The launch body's `"option": "<id>"` picks one (`"game"` for the game's own program); `404 launch_option_not_found` for an unknown one, `400 alongside_option` for an alongside one. A `tool` answers `{"status": "opened", "tracked": false}`, or `500 cgroup_unavailable` without a systemd user session, as a game would.

### `GET /v1/games/{id}/sessions?limit=`
`{"sessions": [{"started_at", "ended_at", "duration_seconds", "exit_code", "signal", "incomplete"}, ...]}`: the game's finished play sessions, newest first, at most `limit` (default 50; `400 invalid_param` unless a whole number, 1 or more). `incomplete` is a session mirad restarted during, so how it ended is unknown. Removing the game removes its sessions.

### `GET /v1/games/{id}/log?lines=`
`{"lines": [...]}`: the last `lines` (default 200; `400 invalid_param` unless a whole number, 1 or more) lines of the game's log, which holds its output plus `mira-run`'s own notes, or its last installer run's output (see install below). Only the last 4 MB of the file is read. A game with no log returns an empty list. Each launch rotates the log to `.log.1`, unless it is over `launch.log_max_mb`.

### `GET /v1/logs/{channel}?after=&lines=`
A live log, one per task so two running at once don't mix. Channels: `daemon` (mirad's own output), `game:<id>` (the game's log file, as above), `setup:<source>` (a launcher's install or a store tool's download), `install:<source>:<ref>` (a store title's install or update) and `runner:<kind>:<name>` (a runner download). Reply: `{"lines": [...], "next": N, "active": bool}`. Send `next` back as `after` to get only what came since; with no `after` the last `lines` (default 300) are returned. `active` is whether the task is still writing. A channel nothing has written to is an empty list. `400 invalid_param` for a bad `after` or `lines`. Non-game channels are kept in memory (the last 4000 lines) and start over when the task starts again.

### `POST /v1/games/{id}/stop`
Sends SIGTERM to the game's process group and every process in its prefix, then SIGKILL after `launch.stop_timeout_s`. Proton games leave the group early, so the prefix is what reaches them. If the game isn't running but has tools open, closes them instead (`{"status": "stopping"}`). If neither is, returns `{"status": "not_running"}` and publishes `game.state` with `idle`. `mirad` also publishes `idle` for every game at startup.

### `POST /v1/open-url`
Body `{"url": "scheme:..."}`. Opens a link whose scheme an app's launcher takes (Autodesk's `adskidmgr:` and `adsk.idmgr:`, which finish its sign-in) with the command the app's prefix registers for it, in the prefix of a running app of that launcher, else the one played last. Mira's `mira.open-url.desktop` entry, `mira open-url %u`, is what the desktop opens these links with. `{"status": "opened", "game_id"}`, `400 invalid_body`, `404 no_app_for_url` when no installed app takes the scheme, `404 no_url_handler` when its prefix registers nothing for it.

### `POST /v1/games/{id}/run`
Body `{"exe_path": "...", "args": "..."}`. Runs any executable in the game's prefix with normal tracking, provisioning the prefix first if there isn't one. This is how an installer is run by hand. `409 program_missing` when `exe_path` (relative to the game's folder, or absolute) isn't a file; a Windows path such as `C:\...` isn't checked.

A native program without its executable bit is `409 not_executable`. A launch or run whose program still can't be started (its interpreter is missing, exec is refused) answers `409 exec_failed` rather than `200`.

### `POST /v1/games/{id}/install`
Body (optional) `{"interactive": bool, "installer": "path", "target": "library root"}`. Runs a `needs_install` game's installer in its prefix. `target`, one of `library_roots` (else `400 invalid_body`), makes a prefix not yet in Mira's prefix folders on that root's disk instead, since the game installs into its prefix: for an installer on a disc or a mounted image, which is full or read-only. Without `target`, a game whose folder is outside every library root uses the first one. Inno Setup, NSIS and MSI installers run silently with `install.inno_args`/`install.nsis_args`/`install.msi_args` and `C:\Games\<name>` in the game's prefix as the target, so the installer's folder keeps only the installer; anything else is shown. `installer` (absolute or relative to `install_path`) picks the file and also works for a `broken` game. One installer runs at a time, in a systemd scope of its own (as a game does), so one that runs the system out of memory is killed without mirad. Its command line, output and exit code (and an Inno Setup installer's own log, from `/LOG=`) go to the game's log (`game:<id>`, which is `active` while it runs), replacing the last launch's, which is kept as `<id>.log.1`. Afterwards the game executable is looked for in that target, in `install_path` (only executables the installer wrote, not ones packed beside it, such as a repack's checksum tool) or in new folders under `install.detect_dirs` in `drive_c`. A game found in a new folder moves there, to its own folder rather than a publisher's folder around it (`Program Files/Ubisoft/<game>`): `installer_dir` keeps the old one, a game still named after the installer's folder takes the new folder's name, and its metadata is fetched again while `metadata.enabled` is on. A job (kind `install`). Events: `game.install.started`/`finished`/`failed` and `game.updated`, also for an installer a scan runs on its own (`scan.auto_run_installers`). Errors: `409 not_needs_install`, `409 install_running`, `404 installer_missing`.

### `GET /v1/games/{id}/installer[?path=]`
`{"path", "size_bytes", "estimated_install_bytes", "format": "inno"|"nsis"|"msi"|"unknown", "silent", "silent_args", "targets": [{"path", "free_bytes"}]}` for the game's installer, or for `path`. `estimated_install_bytes` is a guess at what the game takes once installed, from the storage line of Steam's system requirements in its metadata (the larger of minimum and recommended), -1 without one. `targets` lists the library roots to pass as the install's `target` when the game's folder is outside every one of them (a disc or a mounted image), the first being the default; empty otherwise.

### `DELETE /v1/games/{id}/installer`
Deletes the game's `installer_dir` and clears it, returning the game. Only inside a library root, and never when that folder also holds the game's `install_path` or `data_dir` (`409 installer_dir_in_use`). `404 no_installer_dir` when there's none, `409` while the game runs.

### `GET /v1/games/{id}/install/progress`
`{"state": "idle"|"queued"|"running"|"finished"|"failed", "mode": "silent"|"interactive", "started_at", "finished_at", "error", "bytes_written", "estimated_bytes", "progress"}`. Silent installers report no percentage, so `bytes_written` is the progress signal. `estimated_bytes` is the installer's `estimated_install_bytes`, and `progress` (0..1) is `bytes_written` against it while it runs, kept under 1 since the guess can be low; -1 for either without a guess. Kept in memory only.

### `POST /v1/games/{id}/finish-install`
Marks a `needs_install` or `broken` game `ready` once `exe_path` points at the installed game. `409 no_executable` if `exe_path` is empty, still an installer, or missing.

An optional body `{"install_path"?, "exe_path"?}` switches the game to a program installed in its prefix first: `install_path` must be inside the game's `data_dir` (`400` otherwise, `409` while the game runs), and the game's candidates are detected again there. The move is handled like an install's: `installer_dir`, the name and a metadata refetch.

### `POST /v1/games/{id}/relocate`
Body (optional) `{"install_path"?, "data_dir"?}`. Moves the game's files and prefix to those paths, leaving one left out of the body where it is, or with no body into Mira's layout (`relocate.install_root` or the first library root, and `prefix_root`, or the [prefixes folder](#library-folders) of the game's disk when that's another, named per `prefix_naming`). Targets must be inside a library root, `prefix_root` or another disk's prefixes folder. Store games keep their install folder unless one is given, since their store tool tracks it; Lutris games don't: once moved they become `manual` games (their `source_ref` is `lutris:<slug>`), which a later Lutris import leaves alone. When one folder is inside the other (a prefix holding the game), the outer one moves and the inner one follows. A game run from an AppImage moves as that file alone, into a folder of its own, whatever else its folder holds. Otherwise an install folder that holds another game or a library root isn't moved (`shared_folder`). A program the game only runs (an executable outside the game's folder, or an AppImage handed the game's file in `args`, like an emulator) stays where it is. Moves across filesystems copy then delete, unless `relocate.allow_copy` is off. A [job](#jobs) whose result is the moved game; publishes `game.updated`.

### `POST /v1/games/relocation`
What moving each game into Mira's layout (`relocate` with no body) would do, to show before asking: `{"ids": [...]}` answers a list, in order and without unknown ids, of `{id, name, files, prefix}`, or `{id, name, error}` with the error envelope's fields when the move would be refused (`shared_folder`, `no_library_roots`, `cross_device` with copying off). `files` and `prefix` are each `{action, from, to, reason, copies}`: `action` is `move` (`from` goes to `to`; `copies` when that's another disk, so it's copied then removed), `follow` (it's inside the other one and moves with it), `stay` (`reason` says why, such as "Steam keeps track of them"), `in_place` (already there) or `none` (the game has no such folder). Nothing moves. Sizes come from `POST /v1/games/removal/sizes`, which measures the same folders.

### `GET /v1/games/{id}/tricks`
`{"installed": [...]}`: the verbs winetricks has installed in the game's prefix, in the order it ran them, from the `winetricks.log` it keeps there. Empty for a game without a prefix.

### `POST /v1/games/{id}/tricks`
Body `{"verb": "corefonts"}`. Runs `winetricks --unattended <verb>` in the game's prefix. Fails if the game has no provisioned Wine or Proton prefix or winetricks isn't available (see `/v1/runners/tools`). A job (kind `tricks`) that runs one at a time. Events: `tricks.started`/`finished`/`failed`.

## Library

### Library folders

`library_roots` is an ordinary setting. Changing it (through the API or a hand edit) also updates the watcher. A folder taken out marks the games found in it (`scan` and `manual` ones) `missing`, as if their drive were unplugged: a scan after it's added back restores them, and when the same folders turn up in another library folder (the drive mounted elsewhere) they're followed there with their history, their prefix too when it moved along under the same mount point.

A Windows game on another disk than `prefix_root` gets its prefix on its own disk, so Proton's links between the game and its prefix never cross disks: in the `prefixes` folder of the Mira folder on that disk (whether or not the library folder is inside it), or with none there, in a `prefix` folder inside the game's own folder, so each prefix lives, moves and is deleted with its game. Scans never look in a prefix for games.

### `POST /v1/library/scan`
Scans every library root now: adds new games (each folder in a root or in one of its [sorting folders](#folders-by-tag), and each AppImage loose in one), follows games whose folder was moved by hand within the library folders, marks vanished ones `missing` (or removes them with `library.remove_missing`; a root that can't be read marks nothing), restores ones that came back, and provisions games still waiting on a runner. Each change publishes its own event. A [job](#jobs) whose result is `{"added": 1, "missing": 0, "restored": 0, "moved": 0}`. The watcher runs the same scan on its own when a root changes, but only for new arrivals.

### `POST /v1/library/relocate`
Body (optional) `{"ids": [...]}`. Relocates those games, or every game without a body, into Mira's layout (inside the game's [sorting folder](#folders-by-tag) when the destination is sorted by tag), one at a time, publishing `game.updated` and `job.progress` as each one moves. A [job](#jobs) whose result is `{"moved": N, "failed": N, "errors": [{"id", "error": {...}}]}`.

### `POST /v1/library/import/classify`
Body `{"path": "/absolute/path"}`. Says what a dropped file or folder is, without moving it: `{"kind": "game" | "app" | "unknown", "name", "reason", "programs": [{"path", "platform", "installer"}], "installer"}`. An AppImage is read from its own desktop entry (its `Categories`); anything else is looked up on Steam's store by exact name (software or game). `programs` are what could start it, best first: for a folder its executables scored the way a scan scores them (`path` relative to it, up to 8), for a file the file itself. `installer` marks one an installer builder made (or an `.msi`, or the scan's name and size fallback); the top-level `installer` is the first program's. `installer_info` (`{path, size_bytes, format, silent, targets}`, as `GET /v1/games/{id}/installer` gives them) describes running a file, or a folder's first program when that's an installer, as one before there's a game to ask about. 400 `invalid_body` for a missing or relative path, 404 `not_found` when it doesn't exist.

### `POST /v1/library/import`
Body `{"path": "/absolute/path", "kind": "game" | "app"}`. Moves the file or folder into the first games folder (`game`) or the Applications folder (`app`) in `library_roots`. A [job](#jobs) of kind `import_path` whose result is `{"path"}`, the new location (from another drive it is copied beside the root first, then the original is removed); the watcher then scans it. 400 `invalid_body` for a bad body or kind. The job fails with `source_missing`, `no_library_root` (no games or Applications folder in `library_roots`), `already_in_library` (the path is inside a library root already) or `target_exists`.

### Folders by tag
`tags.sorted_roots` lists the library folders whose games are sorted, and `tags.folders` the tags that get a folder in each of them: `{"tags": {"sorted_roots": ["~/Mira/Games"], "folders": ["RPG", "Strategy"]}}`. A library folder not listed is never touched; with no folder tags, a listed one sorts only hidden games. A game's place is `<root>/[.hidden/][<folder tag>/]<its folder>`: the folder tag is the game's own pick (`folder_tag`, below) while it has that tag and it's a folder tag, else the first tag in `tags.folders` (in that order) the game has, matched ignoring case and spelled as listed; `.hidden` holds games tagged `hidden`. So reordering `tags.folders` moves the games with several folder tags and no pick. `favorite`, `hidden` and `app` can't be folder tags, and a tag can't contain `/` or start with `.`.

- Only `scan` and `manual` games directly in that shape move; store installs, programs a game only runs, and folders put deeper by hand stay. A loose AppImage moves as its file.
- A game installed inside its prefix (under `prefix_root` or its own `data_dir`) never moves: it gets a link at its place instead, named after the game, in the Applications root for an `app` and the first other root for a game. The link moves with its tags, goes away when sorting is turned off or the game is removed, and is stored as `library_link`. Scans and the watcher never follow such a link or add one as a game; a link someone made to a game folder elsewhere is a game like any other.
- A tag change (either PATCH, or a [`/v1/tags`](#tags) call), a change to `tags.folders`, `tags.sorted_roots` or `library_roots` (by a request or a hand edit of `settings.toml`), a game's exit and mirad's start each sort what's out of place, as a `relocate` [job](#jobs) whose result is `{"moved": [...], "failed": [{"id", "error"}]}`; for one game its failure is the job's. Nothing starts when nothing has to move. Moves are renames only: a target that exists (`target_exists`) or is on another drive (`cross_device`) leaves the game where it is. A running game is sorted once it exits.
- The reverse holds too: a scan or the watcher follows a game whose folder (or link) was moved by hand within the library folders and changes its tags to the new place. A new game found in a sorting folder gets its tags the same way. A folder is matched to a game whose folder is gone by its executable inside, then by every executable the detector saw for it, then by the same folder name. When that still leaves several, the folder is an unclear move: it isn't added, those games aren't marked missing, and `library.move_unclear {folder, games: [{id, name}]}` is published until someone settles it (below). A game whose whole sorting folder was deleted is missing like any other. With `library.remove_missing` on, a game whose folder turned up in another library folder is only marked missing, so that folder's scan follows it with its history.
- A sorting folder a game leaves (moved, followed or deleted with its files) is removed once no game is inside and it holds nothing but file-manager leftovers (`.directory`, `.DS_Store`, `Thumbs.db`, `desktop.ini`). A scan alone never removes one, so an empty folder made to drag games into stays.
- A game's `folder_tag` is the folder tag picked for it over `tags.folders`' order, empty to follow that order. Set it with `PATCH /v1/games/{id}` or the batch `PATCH /v1/games` (`""` clears it). It's ignored while the game lacks that tag, and cleared when the tag comes off the game, so adding the tag back later follows the order again. `/v1/tags/rename` renames it. A folder moved by hand into a folder tag's folder sets it, unless the order already puts the game there.
- Each game record has `sort_root`, the library folder (as `library_roots` spells it) the game is sorted in, or `null` for one that never moves; `folder_tags`, that folder's folder tags in order, or `null` while it isn't sorted; and `folder`, the folder tag the game's folder is in, or `null`. A change to `tags.folders`, `tags.sorted_roots` or `library_roots` publishes one `games.updated` with every record, since any of them may now say something else.

### `GET /v1/library/unclear`
The unclear moves scans found: `{"moves": [{"folder", "games": [{"id", "name"}]}]}`.

### `POST /v1/library/unclear`
Body `{"folder": "...", "id"?: "..."}`. Settles an unclear move: with `id` (one of its games) the folder becomes that game's, with its tags following the new place; without, it's added as a new game. The library folders of the other games it could have been are scanned again before it returns, so each is marked missing or followed to where it went. Returns the game; publishes `library.move_settled {folder}`, which is also published when an unclear folder disappears on its own. `404 move_not_found` for a folder that isn't one.

### Mira folders and drops
`mira_folders` lists the Mira folders, `~/Mira` by default, one per disk; each holds the usual Games, Applications and other folders, and adding one is a PATCH of `mira_folders` with its Games and Applications in `library_roots`. The watcher watches the top of each Mira folder. Something new there (a file or folder other than the folders settings name or hold, Games and the rest, or a dot-name) is sorted once its size settles: an archive is extracted beside it (with `scan.auto_extract_archives`) and the result sorted; a program, or a folder holding one, goes into that Mira folder's Games or Applications by the same guess as `/v1/library/import/classify`; anything else that one added emulator runs goes to it (moved). When the guess is unknown, or more than one emulator runs it, Mira publishes `library.drop_unclear {path, name, reason, choices: [{id, label}]}` (a choice's `id` is `game`, `app` or an emulator's id) and leaves it where it is. Anything else stays where it is. What's there when mirad starts is sorted the same way.

### `GET /v1/library/drops`
`{"drops": [{path, name, reason, choices}]}`: the unclear drops waiting for an answer.

### `POST /v1/library/drops`
Body `{"path": "...", "choice": "..."}`. Moves an unclear drop where `choice` (one of its choices) says, as a [job](#jobs) of kind `sort_drop`, and publishes `library.drop_settled {path}`, which is also published when it disappears on its own. The job fails with `drop_not_found`, `invalid_choice`, or the error of the import or emulator add.

### `GET /v1/library/size?source=&ref=`
What installing a store title downloads and then takes on disk, asked of its store tool: `legendary info --json` for Epic, `gogdl info` (its all-languages size) for GOG, `nile install --info --json` for Amazon. `{"download_bytes", "install_bytes"}`, -1 for one the store doesn't give. Kept while mirad runs (up to 500 titles), so asking again is free, and an install of that title reports its `total` from it. `400 invalid_param` without both, `400 size_unsupported` for another store, `404 size_unknown` when the tool's answer has no size, and the tool's own error (signed out, missing) otherwise.

### `GET /v1/library[?source=epic|steam|gog|itch|amazon|office][&fresh=1]`
What each account owns, whether or not it's installed:

```json
[{ "source": "epic", "ref": "e8bbb84be35640cda646233152ff3428", "title": "Brotato",
   "installed": true, "game_id": "epic-e8bbb84be35640cda646233152ff3428",
   "play_seconds": 0, "owned": true, "hidden": false }]
```

`ref` is the store's id and what `/v1/library/install` takes. `installed` and `game_id` say whether Mira tracks it as `<source>-<ref>`. `hidden` is the game's `hidden` tag once installed, and before that whether the title was hidden with `POST /v1/library/hidden`. `play_seconds` comes from the store (only Steam reports it). `owned` is false for a paid itch game listed from a collection the account hasn't bought. `store_url` is the title's store page, for Steam, itch and GOG (the others don't give one). `protondb_tier`, `steam_tags` (most voted first) and `steam_reviews` (`{score_description, percent_positive, total_reviews}`, Steam's user reviews of the title or, with `metadata.steam_by_name`, of the Steam game of its name) are there when cached.

Each source's owned titles are stored in cache.db as it last listed them, so the listing answers at once; titles become games once installed. A source never listed yet is asked during the request. Every other one is re-checked in the background, one check per source at a time, between `library.catalog_checking` and `library.catalog_checked` (`changed` says whether the list differs, the cue to list again). `fresh=1` asks each source during the request instead. Signing out of a store drops its stored list. A source that isn't set up lists nothing, and `GET /v1/<source>/status` tells why. Steam needs `steam.web_api_key` and `steam.steamid64` to list games that aren't installed. Humble Bundle isn't included.

### `POST /v1/library/hidden`
Body `{"source": "epic", "refs": ["..."], "hidden": true}`. Hides or shows owned titles. An installed title's game gets or loses the `hidden` tag (publishing `game.updated`); any other title is kept hidden in mira.db, so its game is tagged `hidden` when it's installed, and removing a hidden game keeps its title hidden. Answers and publishes `library.hidden_changed` with the body.

### `POST /v1/library/install`
Body `{"source": "...", "ref": "..."}`. Installs an owned title as a job (kind `install`, target `<source>-<ref>`).

- `epic`: `legendary install` into `epic.install_root`.
- `gog`: `gogdl download` into `gog.install_root/<id>`.
- `itch`: butler's install sequence.
- `amazon`: `nile install` into `amazon.install_root`.
- `steam`: opens `steam://install/<appid>`. The game appears on the next Steam scan.
- `office`: the Office Deployment Tool adds the app to Microsoft 365's prefix.

A missing tool or login fails the install with `library.install.failed`, whose `code`, `hint` and `fix` say what's needed. Afterwards the title is imported and provisioned. Events: `library.install.started`/`finished`/`failed` and `library.install.progress` (`progress` 0..1, `eta` seconds, `bps`, `downloaded` and `total` bytes of the download, `total` from the tool's own count or else [the size](#get-v1librarysizesourceref) asked before; -1 when not reported) for Epic, GOG, Amazon and itch.

Installing a title again while it's paused resumes it: the store tool continues from the files it left.

### `POST /v1/library/install/pause`
Same body. Stops a running Epic, GOG or Amazon install or update and keeps its files: legendary, gogdl and nile get SIGTERM, then SIGKILL after 3s. Answers `{"status": "pausing", "job": "<id>"}`; the job ends as `cancelled` and `library.install.paused {source, ref, update}` follows. Other stores return `409 pause_unsupported`, and a title with no install running `409 not_running`.

### `GET /v1/library/install/paused`
The paused installs, `[{"source", "ref", "update"}]`. Kept in memory: after mirad restarts the list is empty, but installing the title again still resumes it.

### `DELETE /v1/library/install/paused?source=&ref=`
Forgets a paused install, publishing `library.install.failed` with code `cancelled`. Its files stay, as with a cancelled install. `404 not_paused` if it isn't paused.

### `POST /v1/library/update`
Same body and events as install. Steam returns `400 unsupported`.

### `GET /v1/library/artwork?source=&ref=`
A not-installed title's cached cover, or `404 artwork_not_found`. It is cached under `<source>-<ref>`, so the game keeps its cover once installed.

### `GET /v1/library/metadata?source=&ref=`
A not-installed title's cached details, shaped like `GET /v1/games/{id}/metadata`, or `404 metadata_not_found`.

### `POST /v1/library/artwork`
Body `{"source": "epic", "titles": [{"ref": "...", "title": "..."}]}`. Queues a fetch for each title missing its cover, or its details (with `metadata.title_details` on) or whose details are older than `metadata.refresh_days`, and not already queued. A title gets a cover within 450x675 and its store info, reviews and ProtonDB tier. Steam titles are fetched 50 to a store request. Returns `202 {"queued": n}`, which is 0 when `metadata.enabled` is off. Covers come from the store where possible (Steam's store API, Legendary's and nile's cached art, GOG Galaxy's games database for GOG, itch and Amazon), otherwise from SteamGridDB. Events: `library.artwork_ready`/`artwork_failed`.

## Tags

Tags are a game's `tags`; `favorite`, `hidden` and `app` have their own actions, so these endpoints refuse them and leave them out of their lists. Tags match ignoring case. Tags Mira makes (a Lutris category) are in Title Case as Steam names its tags: each lowercase word capitalised except short joining words ("Free to Play"), other casing kept ("RPG"). Tags a client sets are kept as given; a library from before this was cased the same way once, merging tags that then matched. With `tags.steam` on, a game's or store title's [metadata](#metadata) fetch also stores its Steam tags (the 20 Steam shows, most voted first) in its metadata record as `steam_tags`, from the same Steam store request as its details: a Steam game's by its appid, any other game's (with `metadata.steam_by_name`) by a Steam game of exactly its name. A store's launcher (`source` `launcher`) gets none. Clients offer them as tags, and with `tags.steam_new_games` a game just added (scanned, imported, installed or added by hand) gets, once its first fetch is in, each of its Steam tags that is already one of the library's tags or in `tags.folders`, spelled as the library has it (`game.updated` follows). Other Steam tags are never added to a game.

### `GET /v1/tags`
`{"tags": [{"name", "count", "ids", "folder", "steam_ids"}], "steam": [{"name", "count", "ids"}], "steam_missing": N}`. `tags` are the library's, most games first, spelled as first seen, with the games that have each, whether it's in `tags.folders` (a folder tag no game has yet is listed with none), and the games Steam gives it. `steam` is every Steam tag on the library's games that isn't one of `tags`, most games first. `steam_missing` counts the games with a metadata record but no Steam tags in it yet (fetched before `tags.steam`, or Steam didn't answer).

### `POST /v1/tags/fetch`
A [job](#jobs) (kind `tags`) that fetches the Steam tags of the games `steam_missing` counts, batched; result `{"fetched": N}`. A game with no Steam match is stored with none, so it isn't asked again.

### `POST /v1/tags/set`
Body `{"name": "...", "ids": [...], "folder"?: bool}`. Afterwards exactly the games in `ids` have the tag: it's added at the end where missing and taken off every other game. `folder` also adds it to `tags.folders` or takes it out (publishing `config.changed`). Returns `{"games": [...]}` with the games that changed, publishes `games.updated`, and sorts their folders.

### `POST /v1/tags/rename`
Body `{"from": "...", "to": "..."}`. Every spelling of `from` becomes `to` in place on every game (a game that already had `to` keeps one), and in `tags.folders`, so its folder is renamed too. Returns and publishes like `set`.

### `POST /v1/tags/remove`
Body `{"name": "..."}`. Takes the tag off every game and out of `tags.folders`. Returns and publishes like `set`.

### `POST /v1/tags/preview`
Body `{"folders"?: [...], "sorted_roots"?: [...], "tags"?: {"<id>": [...]}}`: `tags.folders` and `tags.sorted_roots` as they would be, and games' tags as a client is editing them, none of it saved. Returns `{"moving": [{"id", "name", "to"}]}`: the games (and links) that would move, and where to, so a client can say what a change does before asking for it. Folder tags the setting would refuse are `400 invalid_setting`.

## Hidden games

Hidden games are those tagged `hidden`, plus store titles hidden with `POST /v1/library/hidden`. With a password set they start locked each time mirad starts, and while locked mirad answers as if they weren't there:

- **Lists leave them out.** Every list skips them, whatever is asked for: `GET /v1/games` (even with `include_hidden` or `tag=hidden`), `/v1/tags` (their ids and counts), `/v1/library`, `/v1/sources` counts, `/v1/steam/installed`, `/v1/library/unclear`, `/v1/library/install/paused` and `/v1/tags/preview`.
- **Naming one is a missing game.** A request that names one answers `404 game_not_found`, as for a game that doesn't exist. That covers `/v1/games/{id}...`, `source` with `ref` in the query, and `id`, `ids`, or `source` with `ref`, `refs` or `titles` in the body. Their jobs answer `404 job_not_found`.
- **`POST /v1/tags/set` leaves their tags alone.**
- **Events about them are dropped.** A game hidden while locked is published as `game.removed` (or `games.removed`). Buffered events about them become `concealed` events with no payload when hidden games lock, so a reconnecting client isn't replayed them.

Only a crypt(3) hash of the password is kept (yescrypt from the system's libcrypt, in mira.db). With `hidden.lock_after_minutes`, they lock again once no client has reported activity for that long, unless a game is running. Every change of state publishes `hidden.changed` with `{password, locked}`, the cue for a client to fetch its games again.

### `GET /v1/hidden`
`{"password": bool, "locked": bool}`.

### `PUT /v1/hidden/password`
Body `{"current"?: "...", "password": "..."}`. Sets the password, changes it, or with `""` removes it. Once one is set, `current` must match it (`403 wrong_password`, after a pause). Leaves hidden games unlocked. Answers like `GET`.

### `POST /v1/hidden/unlock`
Body `{"password": "..."}`. `403 wrong_password` after a pause, or answers like `GET`.

### `POST /v1/hidden/lock`
Locks hidden games now. Answers like `GET`.

### `POST /v1/hidden/left`
A client left hidden mode: locks hidden games when `hidden.lock_on_leave` is on. Answers like `GET`.

### `POST /v1/hidden/activity`
Someone is using a client: restarts `hidden.lock_after_minutes`' count. `204`.

## Runners

### `GET /v1/runners`
Every installed build, discovered on each call:

```json
[{ "kind": "proton", "name": "GE-Proton11-7", "label": "GE-Proton11-7",
   "path": "/home/x/.steam/steam/compatibilitytools.d/GE-Proton11-7-x86_64",
   "version": "1789520217", "release": "GE-Proton11-7", "reference": "proton:GE-Proton11-7",
   "source": "proton_ge", "removable": true }]
```

`reference` is what `runner_ref` and `default_runner.*` use. `release` is the name the build's own files give. A Proton build is named by it, except one its owner replaces in place (a distro package under `/usr` or `/opt`, Steam's own Proton in `steamapps/common`), which is named by its folder (`proton-cachyos-slr`) so references survive its updates. At startup, references to such a build's older release names are moved to the folder name, when exactly one build matches: the same major version, or any version for a folder without one in its name. `label` is a readable name, such as "Wine 11.18 staging-tkg". `source` is the download source the build matches, or empty. `removable` is false for builds outside `runner_search_paths`/`wine_search_paths`, such as distro, Steam or system builds.

Discovery also looks where Steam, the distro, Heroic, Bottles and Lutris keep builds, plus `/opt/*`, unless `runner_scan_common_dirs` is off. Proton builds only appear when `umu-run` is available. A build reachable through several paths is listed once. `native` and `steam` have no builds and never appear.

### `GET /v1/runners/sources?kind=proton|wine`
Download sources, preferred first: `[{"id": "proton_ge", "kind": "proton", "label": "GE-Proton"}]`.

- Proton: `proton_ge`, `proton_cachyos`, `proton_umu`, `proton_em`, `proton_sarek`.
- Wine: `wine_staging_tkg`, `wine_staging`, `wine_vanilla` (Kron4ek), `wine_ge`, `wine_lutris`.

GE-Proton and Wine-GE read their repo and asset pattern from `runner_sources.*`.

### `GET /v1/runners/catalog?kind=&source=`
Releases from one source (the kind's first by default), newest first, cached for 10 minutes to stay under GitHub's rate limit:

```json
[{ "tag": "GE-Proton11-7", "name": "GE-Proton11-7", "label": "GE-Proton11-7", "source": "proton_ge",
   "asset_name": "GE-Proton11-7.tar.gz", "size_bytes": 563784602,
   "published_at": "2026-09-16T02:28:16Z", "has_checksum": true, "installed": true }]
```

`name` identifies the release in events: the tag for Proton, the archive name for Wine.

### `POST /v1/runners/download`
Body `{"kind", "tag", "source"?}`. Downloads a release into the first search path of its kind, checking its `.sha512sum`, `.sha256sum` or `sha256sums.txt` when there is one; a mismatch discards the download. A job (kind `runner`). Events: `runners.download.started`/`finished`/`failed` with `{kind, tag, name, label, source}`, and `runners.download.progress` with those plus `progress` (0 to 1) as each percent arrives, when the release's size is known. When a download finishes, games left broken by a missing runner are provisioned again.

### `GET /v1/runners/updates`
Removable builds whose source has a newer release:

```json
[{ "reference": "wine:wine-11.17-staging-tkg-amd64", "source": "wine_staging_tkg",
   "tag": "11.18", "name": "wine-11.18-staging-tkg-amd64", "label": "Wine 11.18 staging-tkg" }]
```

### `POST /v1/runners/update`
Body `{"reference": "kind:name"}`. Installs the newer release like a download, then moves everything naming the old build onto the new one: games' runners, their overrides and every runner setting (`default_runner.windows`, a store's `<id>.runner`, ...). The old build stays. The moved games arrive as `games.updated`, then `runners.updated {kind, from, to, games}` fires before `runners.download.finished`, which then carries `replaced`. `409 no_update` when nothing is newer.

### `GET /v1/runners/tools`

```json
[{ "id": "umu", "label": "umu-launcher", "installed": true, "path": "/usr/bin/umu-run", "doc": "..." },
 { "id": "winetricks", "label": "winetricks", "installed": false, "path": "", "doc": "..." }]
```

A copy on `PATH` wins over Mira's own in `~/.config/mira/tools`.

### `POST /v1/runners/tools/{umu|winetricks}/setup`
A job (kind `setup`) that installs the latest umu-launcher zipapp (needs python3) or winetricks script into `~/.config/mira/tools`. Events: `umu.setup.*` or `winetricks.setup.*`.

### `DELETE /v1/runners/{kind}:{name}`
Removes a build that lives inside a search path. `400` for system builds, `auto`/`latest`, or kinds without builds; `404` if the build isn't installed. Publishes `runners.removed`.

### `GET /v1/runners/{kind}/schema`
What `runner_config` accepts for a kind: `[{"key": "gameid", "label": "Steam game ID", "type": "string", "doc": "..."}]`, `label` in sentence case. Empty for every kind but `proton`. `404` for an unknown kind.

## Steam

Steam games are ordinary games with `runner_ref` `steam:<appid>`.

### `POST /v1/steam/scan`
Reads Steam's `libraryfolders.vdf`, `appmanifest_*.acf` and `compatdata/<id>/config_info` directly and adds or updates installed games. A [job](#jobs) whose result is `{"added": 2, "updated": 0}`. A rescan updates `name`, `install_path` and `data_dir` and leaves user settings alone. `exe_path` is never filled in, because Steam keeps the launch command in its `appinfo` cache; only `steam.launch_mode: "direct"` needs it.

### `GET /v1/steam/accounts`
Whether Steam is installed, and the accounts that have signed in to it on this computer, from `config/loginusers.vdf`: `{"found", "accounts": [{"steamid64", "account_name", "persona_name", "most_recent"}], "selected"}`. The one Steam flags as most recent comes first, then the newest sign-ins. `selected` is `steam.steamid64`, or the first account while that's empty, which is the account owned games and playtime are read for. Without Steam, `found` is false and both are empty.

### `GET /v1/steam/installed`
The games Steam has installed here, read the same way as `POST /v1/steam/scan` but without adding them: `{"found", "games": [{"appid", "name", "last_played_at"}]}`, the most recently played first (from the `selected` account's `localconfig.vdf`). `last_played_at` is left out for a game never played. Without Steam, `found` is false and `games` is empty.

### `POST /v1/steam/shortcut`
Body `{"exe", "launch_options"}`. Keeps a non-Steam shortcut named "Mira" running `exe` with `launch_options` in every Steam account's `userdata/<id>/config/shortcuts.vdf`, so Big Picture can switch to Mira. An existing "Mira" entry is updated in place, other shortcuts are kept, and an unchanged file isn't rewritten. Answers `{"status": "ok", "added": [ids], "updated": [ids]}`, or `{"status": "disabled"}` with `steam.mira_shortcut` off. `404 steam_not_found` without Steam, `500 shortcuts_unreadable` for a file Mira can't parse (it's left alone). Steam shows a new shortcut after it restarts. The GUI calls this at start with its own path and `--big-screen`.

### `POST /v1/steam/bigpicture`
Opens Steam's Big Picture through `steam steam://open/bigpicture`, starting Steam if needed. Answers `{"status": "opened"}`.

### `POST /v1/steam/status`
Body `{"status": "online" | "invisible"}`. Sets the Steam friends status through `steam steam://friends/status/<status>` and answers `{"status": "invisible"}`. Steam can't report the status back, so there is no GET. Answers 409 `steam_not_running` while Steam isn't running (by `~/.steam/steam.pid`), since the URL would start the client just to set a status, and 400 `invalid_status` for any other value.

## Lutris

### `GET /v1/lutris`
Whether Lutris is on this computer: `{"found", "data_dir"}`, found by its `pga.db` (`lutris.data_dir`, else `$XDG_DATA_HOME/lutris`). `data_dir` is empty when not found.

### `POST /v1/lutris/import`
Reads Lutris's `pga.db` (through the `sqlite3` CLI) and each game's YAML config (`lutris.data_dir` overrides where to look) and imports `wine` and `linux` runner games. A [job](#jobs) whose result is:

```json
{ "added": 3, "updated": 1, "other_runner": 2, "incomplete": 0 }
```

- `linux` games import as native with no prefix.
- `other_runner` counts games using other runners, which are skipped. Steam and Flatpak games are covered by the Steam scan and desktop entry import.
- `incomplete` counts Wine games with no `prefix` in their config and Linux games with a relative `exe`.

Nothing on disk is moved. `install_path` is the executable's folder and `data_dir` is the configured prefix. `runner_ref` is left empty so `default_runner.windows` applies, since Lutris's Wine version is often an alias. Lutris categories become tags in Title Case (`.hidden` becomes `hidden`, `favorites` becomes `favorite`) and are merged with existing tags, ignoring case. Re-importing updates Lutris's fields and leaves overrides alone. Lutris sets `install_path`, `exe_path` and `data_dir` on the first import; after that Mira's stay (a different executable picked in Mira survives) unless what they point at is gone. `candidates` lists the executables in the game's folder, unless the folder holds other games too. Lutris's `playtime` and `lastplayed` fill `play_seconds` and `last_played_at` until Mira has recorded a session of its own (`last_session_at`); after that Mira's record stays.

## Heroic

### `GET /v1/heroic`
Whether Heroic Games Launcher is on this computer: `{"found", "config_dir"}`, its config folder (`heroic.config_dir`, else `$XDG_CONFIG_HOME/heroic`, else the Flatpak's `~/.var/app/com.heroicgameslauncher.hgl/config/heroic`). `config_dir` is empty when not found.

### `POST /v1/heroic/import`
Imports the games Heroic installed, from its own files: Epic games from `legendaryConfig/legendary/installed.json`, GOG games from `gog_store/installed.json` (the program from the game's `goggame-<id>.info`, `start.sh` on Linux), Amazon games from `nile_config/nile/installed.json` (the program from the game's `fuel.json`) and sideloaded games from `sideload_apps/library.json`. A [job](#jobs) whose result is:

```json
{ "added": 3, "updated": 1, "unsupported": 1, "incomplete": 0 }
```

- `unsupported` counts macOS and browser games, which are skipped.
- `incomplete` counts games whose program, or a Windows game's prefix (`GamesConfig/<id>.json`'s `winePrefix`, its `pfx` folder for a Proton prefix), Heroic's files don't name.

The games' `source` is `heroic` and `source_ref` is `<store>:<id>` (`epic`, `gog`, `amazon` or `sideload`). They run through Mira's runners in Heroic's prefix, without the store's sign-in, as Lutris's do; `runner_ref` is left empty so `default_runner.windows` applies. A game Mira already has at that folder from its own Epic, GOG or Amazon source stays that source's. Nothing on disk is moved, and as with Lutris, Heroic sets `install_path`, `exe_path`, `args` and `data_dir` on the first import, after which Mira's stay unless what they point at is gone.

## Stores

Epic, GOG, itch, Amazon and Humble each wrap a command-line tool, and all five share one set of calls under `/v1/stores/{id}`, where `id` is `epic`, `gog`, `itch`, `amazon` or `humble` (`404 store_not_found` otherwise). Store games always launch through Mira's own runners, never through the store tool.

- `GET /v1/stores`: `[{"id", "name", "tool_name", "can_import", "can_logout"}]`.
- `GET /v1/stores/{id}/status`: `{id, name, tool, authenticated, account}`. `account` is only known for Epic. `tool` is `{"installed", "source", "path", "version"}`, where `source` is `override` (the `<store>.*_bin` setting), `managed` (Mira's copy in `~/.config/mira/tools`), `path` or `none`, in that order. `version` is the release tag Mira downloaded for a `managed` copy, else what the tool prints for `--version`. A missing tool is not an error. GOG's also has `helper`, the same shape plus `name: "comet"`.
- `POST /v1/stores/{id}/setup`: a [job](#jobs) (kind `setup`) that downloads the tool's latest release. Run it again to update. Result `{tag}`. Setups of one store run one at a time; one asked for while another runs stops once that has installed the tool. GOG's (labelled "Setting up gogdl and comet") also downloads comet's latest release (picked for this machine's processor by `runner_sources.comet.asset_pattern`); a failure there is only written to the setup log, since GOG works without it.
- `POST /v1/stores/{id}/login/begin`: `{url}`, the page to sign in at. Amazon makes a fresh one each time.
- `POST /v1/stores/{id}/login`: body `{"credential": "..."}`. Returns the status. What the credential is depends on the store:
  - Epic: the `authorizationCode`, or the whole page the login ends on (the JSON, or Firefox's JSON viewer copied as text). Legendary exits 0 on a bad code, so the result is checked through status; a rejected code fails with `400 login_failed`.
  - GOG: the `code` from the redirect URL, or the whole URL. An expired token is refreshed once.
  - itch: an API key from [itch.io/user/settings/api-keys](https://itch.io/user/settings/api-keys), checked with butler straight away.
  - Amazon: the amazon.com URL the login ends on, or its `openid.oa2.authorization_code`, after `login/begin`.
  - Humble: the `_simpleauth_sess` cookie from a logged-in browser.
- `POST /v1/stores/{id}/login/find`: body `{"text": "..."}`, text the user copied. Answers `{"credential": "..."}` when it holds what `login` takes, else `{"credential": null}`, so a client can sign in as soon as the right thing is on the clipboard. Stricter than `login`: a bare GOG or Amazon code isn't recognised (only the URL), an itch key or Humble cookie only when the text is nothing but that.
- `POST /v1/stores/{id}/logout`: forgets the sign-in. `400 logout_unsupported` for Humble, whose tool keeps its own session.
- `POST /v1/stores/{id}/import`: a [job](#jobs) (kind `import`) that adds the games the store's tool reports as installed, with the store as their `source`, and provisions a prefix for each. Result `{added, updated}`. `400 import_unsupported` for Humble.

### Epic

Wraps [Legendary](https://github.com/legendary-gl/legendary), whose Linux release since 0.21 is a zipapp that needs `python3` 3.10 or newer. Deleting an Epic game's files runs `legendary uninstall`.

### GOG

Wraps [gogdl](https://github.com/Heroic-Games-Launcher/heroic-gogdl), which needs `python3`. gogdl can't list owned or installed games, so the library listing uses GOG's own API with gogdl's token, and import only looks under `gog.install_root` (default `~/Mira/Games/GOG`). The listing leaves out packs, DLC and other entries that aren't installable games; a game's owned DLC installs with it (`--with-dlcs`) while `gog.install_dlc` is on, and an update adds DLC bought since.

### itch.io

Wraps [butler](https://itch.io/docs/butler/). `mirad` starts `butler daemon` on first use and keeps the connection, so changing `itch.butler_bin` needs a restart.

- `GET /v1/stores/itch/collections`: the collections whose games `GET /v1/library?source=itch` lists: the account's own, then any added by link. `[{"id", "title", "games_count", "own", "url"}]`.
- `POST /v1/stores/itch/collections`: body `{"link": "https://itch.io/c/8213205/..."}`, or a bare id. The collection is read through butler first, so bad or private links are refused. Returns the collection with `201`.
- `DELETE /v1/stores/itch/collections/{id}`: removes a collection added by link.

### Amazon Games

Wraps [nile](https://github.com/imLinguin/nile). Installed games (`amazon-<product id>`) run their `fuel.json` command through Mira's runner with the Amazon SDK variables `nile launch` would set.

### Humble Bundle

Wraps [humble-cli](https://github.com/smbl64/humble-cli). Humble has no installs, only downloads, so it isn't a library source. humble-cli has no JSON output, so its table output is parsed.

- `GET /v1/stores/humble/bundles`: `[{"key", "name", "claimed"}]`.
- `POST /v1/stores/humble/download`: body `{"bundle_key", "item_numbers"?}` (humble-cli's `1,3,5-7` syntax). A [job](#jobs) (kind `download`, target the bundle key) that downloads into `<humble.download_root>/<bundle_key>/`. Result `{bundle_key, path}`. A bundle with nothing to download, such as a Steam key, fails with `nothing_to_download`. Add the result with `POST /v1/games/manual`.

## Store launchers

Battle.net, Ubisoft Connect and the EA app have no Linux client, so each is installed into its own prefix (game `launcher-<id>`). Games installed through a launcher are imported as `<id>-<ref>` with source `battlenet`, `ubisoft` or `ea`, sharing its prefix and runner. Microsoft 365 (`office`) works the same way: its install sets up the prefix with the [mira-winapp-shims](https://github.com/Mira-Launcher/Mira-winapp-shims) DLLs and Microsoft's Edge WebView2 runtime. Its apps (Word, Excel, PowerPoint, Outlook, OneNote, Access, Publisher) are then a library source like a store: `GET /v1/library?source=office` lists them and `POST /v1/library/install` installs one through the Office Deployment Tool, for the edition in `launchers.office.plan`, as `office-<app>`, tagged `app`. Deleting an app's files runs the Office Deployment Tool without it. Office signs in and checks the subscription itself; Mira never sees the account. Adobe Creative Cloud (`adobe`) installs into a prefix on [AdobeWine](https://github.com/le-birnes/AdobeWine), Wine with fixes for Creative Cloud and its apps: unless `launchers.runner` names a runner, the install downloads its newest release, or the fork or release `launchers.adobe.adobewine_url` links to, into Mira's `tools/adobewine` folder, where it is also a Wine runner. Creative Cloud's own entry is tagged `app` too. Creative Cloud's installer is shown and you sign in there; apps it installs into `Program Files/Adobe` (Photoshop, Illustrator, Premiere Pro, After Effects, InDesign, Audition, Media Encoder, Lightroom, Lightroom Classic, Animate, Bridge, Acrobat) import as `adobe-<app>`, tagged `app`, each launched by its own exe. `launchers.auto_import` imports on every scan. umu's `STORE` and, when known, `GAMEID` are set so protonfixes apply.

### `GET /v1/launchers`
`[{id, name, kind, app, installer, game_id, installed, install_state, interactive_install, packages, prefix, runner_ref, error}]`. `kind` is `games` for a game launcher, or `apps` for one that brings applications (Microsoft 365, Creative Cloud, the Apps source). `app` is true for an app of its own (Fusion, AutoCAD, Inventor, 3ds Max, Revit, QuickBooks Desktop), whose entry is tagged `app` and launches it. `installer` is non-empty for one that installs from the user's own installer, and says which one. `install_state` is `idle`, `running`, `finished` or `failed`. `packages` is the feature whose system packages installing it needs (`winetricks`, see `GET /v1/system/packages`), or empty. Microsoft 365 also lists `apps: [{ref, name}]`, installed or not, so they can be picked before Office is.

### `POST /v1/launchers/{id}/install`
Creates the prefix, runs the winetricks steps, then the installer: silent for Ubisoft and EA, shown for Battle.net and Creative Cloud. A launcher with `installer` takes `{"installer": "/path/to/installer.exe"}`, or fails with `400 installer_needed`: Autodesk's Create Installer builds the install image and Mira runs its `Setup.exe` silently, with the wintrust and bcryptprimitives shims in the prefix. Imports games afterwards. A job (kind `install`). `409 install_running`. Events: `launcher.install.*`; `launcher.install.progress` (`progress` 0..1) when the installer reports how far along it is (Microsoft 365 does). An app's prefix uses `launchers.app_runner` (default `proton:GE-Proton11-7`, downloaded when missing).

### `POST /v1/launchers/office/apps`
Body `{"apps": ["word", "excel", ...]}`: Microsoft 365 apps to add to the installed ones, in one run of the Office Deployment Tool. While Microsoft 365 is installing (its prefix, or other apps), they're kept and added when that ends, answering `200 {"status": "queued"}`; a failed install drops them. Otherwise a [job](#jobs) (kind `install`, target `office`). `400 unknown_app`, `409 launcher_not_installed`. A failed run publishes `launcher.install.failed`.

### `POST /v1/launchers/{id}/import`
A [job](#jobs) whose result is `{added, updated}`. Battle.net games are found by their default folders, Ubisoft games by registry keys, EA games by `__Installer/installerdata.xml` and Creative Cloud apps by their folders. `409 launcher_not_installed`.

### `POST /v1/launchers/{id}/open`
Body `{action?: "launch"|"install", ref?}`. Opens the launcher, or asks it to launch or install a game by store id (a Battle.net product code such as `WTCG`, a Ubisoft id or an EA offer id). Not tracked.

## Emulators

Each emulator is a source of its own: DuckStation, PCSX2, PPSSPP, RPCS3, shadPS4, KytyPS5, Dolphin, Cemu, Eden, Ryujinx, Azahar, melonDS, xemu, Xenia Canary, Flycast, Mesen, Snes9x, bsnes, mGBA, ares, BlastEm, RMG, Vita3K, Ymir, Mednafen, Stella, MAME, Mupen64Plus, Redream, atari800, Hatari, FS-UAE, VICE, openMSX, UZDoom, EasyRPG Player, DOSBox Staging and ScummVM. Its games are the files (or, for RPCS3, shadPS4, KytyPS5, EasyRPG Player, DOSBox Staging and ScummVM, the folders; DOSBox starts a folder with the file named like it, else one that isn't its setup, and keeps one the user picks) in `<emulators.roms_root>/<name>`, which the user adds and Mira watches. They import as `<id>-<file>`, Ready, with runner `emulator:<id>`, which runs the emulator with the game's file. Dolphin, ares, Snes9x and MAME come from the system's packages, and Ryujinx from CachyOS's package; the rest download from their releases page (GitHub or Forgejo), or BlastEm's from its site, into Mira's `tools/<id>` folder. A download that needs a library from the system's packages (bsnes's libao) asks for it, at install and at launch. With `emulators.fullscreen` (on by default) games start fullscreen. `emulators.<id>.bin` points at another copy, and one on `PATH` is found. MAME finds an arcade set by its zip's name in the folder. An emulator's AppImage found in a library folder isn't imported as a game: it becomes that emulator's program (when it has none) and its source is added. BIOS, firmware and keys are never downloaded. Removing an emulator keeps its games' files; deleting a game deletes its file and the files a `.cue`, `.gdi` or `.m3u` names.

### `GET /v1/emulators`
`[{id, name, consoles, extensions, folder_games, installed, path, version, package, rom_dir, firmware: [{id, label, required, present}]}]`. `version` is the release Mira downloaded, `package` the system packages feature that installs it (see `GET /v1/system/packages`), empty for one Mira downloads.

### `POST /v1/emulators/{id}/install`
Downloads the latest release over Mira's copy and marks the source added. A [job](#jobs) (kind `install`) whose result is the emulator as above. A package emulator fails with the packages fix. `already_installing`.

### `POST /v1/emulators/{id}/files`
Body `{"paths": [...], "move"?}`: copies the files, and the ones a `.cue`, `.gdi` or `.m3u` names, into the emulator's folder, or moves them when `move` (default: the emulator's `emulators.<id>.add_mode`, else `emulators.move_added`, on), then imports. A [job](#jobs) whose result is `{added, updated, choose}`; `choose` lists added games whose start file Mira couldn't tell (`{id, name, files}`, its pick first), for the client to ask about and set with `PATCH /v1/games/{id}` `exe_path`. Fails with `wrong_files` when the emulator takes none of them.

### `POST /v1/emulators/{id}/import`
Imports its folder again. A [job](#jobs) whose result is `{added, updated}`.

### `GET /v1/emulators/{id}/existing`
Games the emulator had before Mira: `{dirs: [{path, games}], shortcuts: [{id, name, file}]}`. `dirs` are the game folders its own settings list (Dolphin, DuckStation, PCSX2, Azahar, Eden, Cemu, Flycast) outside its folders that hold games; `shortcuts` are desktop entries in the library that run one of its games.

### `POST /v1/emulators/{id}/existing`
Body `{"move": bool}`: takes them in. A desktop entry becomes that game, keeping its id, playtime and art. With `move` the files go into the emulator's folder; otherwise the folders are added to `emulators.<id>.game_dirs`, which Mira imports from and watches as well. A [job](#jobs) whose result is `{added, updated}`.

### `POST /v1/emulators/{id}/firmware/{firmware}`
Body `{"paths": [...]}`: puts a BIOS, keys or firmware where the emulator looks. A zip is unpacked (Switch firmware) and RPCS3 installs a `.PUP` itself. Returns the emulator.

### `POST /v1/emulators/{id}/open`
Opens the emulator alone, to change its settings. Not tracked.

### `GET /v1/games/{id}/emulators`
`[{id, name}]`: the other installed emulators the game could move to, sharing its console and taking its file.

### `POST /v1/games/{id}/emulator`
Body `{"emulator": "<id>"}`: moves the game's files into that emulator's folder. It keeps its id, art, tags and playtime. Returns the game. `already_there`.

## Mod managers
Windows mod managers that cover many games: `vortex` (Vortex) and `mo2` (Mod Organizer 2). Each is a source. Mira downloads it once into `~/.config/mira/tools/<id>`, and adding it to a Windows game makes it one of the game's `tool` [launch options](#launch-options), run in that game's prefix, where it sees the game's files and settings the way the game does.

### `GET /v1/mod-managers`
`[{"id", "name", "installed", "version", "games": [game ids it's on]}, ...]`.

### `POST /v1/mod-managers/{id}/install`
Downloads the latest release over Mira's copy and adds its source. A [job](#jobs). `404 mod_manager_not_found` for an unknown id.

### `POST /v1/games/{id}/mod-managers/{manager}`
Runs the winetricks verbs it needs in the game's prefix (Vortex: `vcrun2022`, `dotnetdesktop9`; Mod Organizer 2: `vcrun2022`), then adds it as the game's launch option with the manager's id. A job on the winetricks lane, since the verbs can take minutes. `409 not_windows_game` for a game without a prefix, `mod_manager_missing` while it isn't installed.

### `DELETE /v1/games/{id}/mod-managers/{manager}`
Takes it off the game. Its settings and mods stay in the prefix. Returns the game.

Removing the source deletes Mira's copy and takes it off every game.

## Updates

### `GET /v1/updates`
What has a newer release: Mira itself, and the emulators, store tools, umu and runner builds Mira downloaded. Copies the system or the user installed aren't checked. Sources that can't be reached are left out.

```json
[{ "kind": "mira", "id": "mira", "name": "Mira", "installed": "0.14.1", "latest": "v0.15.0",
   "url": "https://github.com/Mira-Launcher/Mira/releases/tag/v0.15.0" },
 { "kind": "emulator", "id": "duckstation", "name": "DuckStation", "installed": "latest", "latest": "latest", "url": "" }]
```

`kind` says how to install it: `emulator` through `POST /v1/emulators/{id}/install`, `tool` through `POST /v1/stores/{id}/setup` (or `POST /v1/runners/tools/umu/setup` for `umu`), `runner` through `POST /v1/runners/update` with `id` as the reference. comet is a `tool` with `id` `gog`, since GOG's setup updates it; while GOG's gogdl is Mira's copy and comet isn't there yet (GOG set up before Mira fetched comet), it's listed with `installed` empty. Mira's is the release at `url`. A release that keeps its tag (DuckStation's `latest`) is newer when it was published later. `updates.check` says whether a client should ask.

`emulators.<id>.repo` switches an emulator Mira downloads to a fork: `owner/repo` or a GitHub link, or a Forgejo or GitLab releases API link. The fork's release then no longer matches what's installed, so it shows here as an update.

## Sources

### `GET /v1/sources`
Every source, in sidebar order: stored ones by `position` first, then the rest in default order. `added` is true for `local` always. `games` counts the games it owns. `in_hidden_sidebar` is whether hidden mode's sidebar lists it. While `hidden_sidebar_mirrors` is true, that's `in_sidebar`; the first change made in hidden mode gives it a copy of its own, which changes to `in_sidebar` no longer touch, even once the two match again. `{"sources": [{"id": "steam", "added": true, "enabled": true, "in_sidebar": true, "in_hidden_sidebar": true, "position": 0, "imported_at": 1760000000, "games": 12}], "hidden_sidebar_mirrors": true}`

### `PATCH /v1/sources/{id}`
Body may hold `added`, `enabled`, `in_sidebar` and `in_hidden_sidebar` (booleans); other keys are ignored. Setting `in_hidden_sidebar` while hidden mode mirrors first copies the whole list for it. Returns the source as above and publishes `sources.changed`. `404 unknown_source` for an id not in the list.

### `PUT /v1/sources/hidden-sidebar`
Body `{"mirrors": true}` makes hidden mode's sidebar list what the normal one does again, dropping its own list; `false` gives it a copy of the list as it is now (nothing changes if it has one already). Returns `{"hidden_sidebar_mirrors": ...}` and publishes `sources.changed`. `400 invalid_body` without a boolean `mirrors`.

### `PUT /v1/sources/order`
Body `{"order": ["gog", "local"]}`. Unknown ids are dropped and the rest get positions 1, 2, ... in that order. Returns `{"ok": true}` and publishes `sources.changed`.

### `GET /v1/sources/{id}/removal`
What removing a source would do:

```json
{ "source": "ubisoft", "games": [ { "id": "ubisoft-5595", "name": "Trackmania",
    "deletes": "/home/me/Mira/prefixes/ubisoft-connect/drive_c/.../Trackmania" } ],
  "program_dir": ".../drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher",
  "packages": [], "uninstall": [], "data": [],
  "kept": [ "/home/me/Mira/prefixes/ubisoft-connect", ".../Ubisoft Game Launcher/savegames" ],
  "signs_out": false, "tools": [] }
```

`tools` lists a store's tools Mira downloaded, each `{name, path}` (GOG's gogdl and comet: `[{"name": "gogdl", "path": "~/.config/mira/tools/gog"}, ...]`), deleted after its games are uninstalled and the account signed out. A copy on `PATH` or set by a `<store>.*_bin` setting is left alone.

`deletes` is empty for games that are only dropped from Mira (Steam, Lutris and Humble own their files, and an emulator's games are the user's). For an emulator, `program_dir` is Mira's download of it, `packages` the system packages it came from with `uninstall` the command removing them (empty on a distro Mira doesn't know), and `data` its settings, saves and caches.

### `POST /v1/sources/{id}/remove`
Uninstalls the source's games (through the store tool, or by deleting a folder inside a Mira folder), deletes a launcher's program folder but keeps save folders, signs out, deletes the store tools Mira downloaded, removes the games from Mira and marks the source not added. An emulator's download is deleted, and its `data` too when the body is `{"delete_data": true}`; its packages are left for the client to uninstall as root. Prefixes are never deleted. A [job](#jobs); a failed step is reported and the rest still run: `{"removed": 3, "problems": []}`.

### `GET /v1/sources/{id}/runner`
The runner a source's games use: `{"runner_ref", "games", "differing"}`. `games` counts its Windows games and `differing` the ones on another runner. For Epic, GOG, itch and Amazon this is `<id>.runner`, the default for their games with no runner of their own (empty falls back to `default_runner.windows`). For a launcher it is the runner of its prefix, which its games share. `400 no_runner` for Steam, Lutris and Humble; `409 launcher_not_installed`.

### `POST /v1/sources/{id}/runner`
Body `{"runner_ref", "apply_to_games"?}`. `runner_ref` is `kind:name`, `auto` or empty, and must resolve to an installed build. A store's default changes, plus every Windows game from it when `apply_to_games` is true. A launcher and the games in its prefix always change together. Returns the new state as above and publishes `game.updated` for each game it moved.

## Desktop entries

Imports existing `.desktop` entries as games. Mira's own `mira-<id>.desktop` entries are controlled by the `desktop_entries.*` settings; a hidden game gets none unless `desktop_entries.hidden_games` is on (off by default), so with it off hiding a game removes its entry and showing it again adds it back. Nothing is imported automatically.

### `GET /v1/desktop-entries/candidates`
Entries from `$XDG_DATA_HOME/applications`, `$XDG_DATA_DIRS`, the Flatpak export directories and `desktop_import.extra_dirs`:

```json
[{ "id": "com.spotify.Client", "name": "Spotify", "icon": "com.spotify.Client",
   "command": "com.spotify.Client", "flatpak": true, "app": true }]
```

`id` is the desktop file ID. `command` is the program it runs, or a Flatpak's app id. `app` is true when its `Categories` don't include `Game`. Skipped: non-applications, `NoDisplay` or `Hidden` entries, Mira's own entries, Steam game shortcuts, and entries already in the library.

### `POST /v1/desktop-entries/import`
Body `{"ids": [...], "apps"?: [...]}`. The ids also in `apps` are tagged `app`; the others are games (re-importing one as a game takes the tag off). A Flatpak entry becomes `flatpak run <app-id>`. Anything else uses its `Exec=` line with field codes removed. The command is stored as an absolute `exe_path`: a bare one is looked up on `PATH` when importing, and an entry whose command isn't installed isn't offered. Imported games are native, keep the entry's id in `source_ref`, and get a `desktop_entries.enabled: false` override, since the app already has a menu entry. Importing the same entry again updates it and keeps that override if it's set. Returns `{"added": 1, "updated": 0}`.

### `POST /v1/desktop-entries/sync`
Rewrites Mira's own desktop entries now.

## GameMode

### `GET /v1/system/packages[?for=]`
The system packages Mira's features use and which are missing, for this distribution. `for` is `core` (curl), `wine` (Wine, winetricks), `winetricks` (cabextract, unzip: winetricks verbs and store launchers like Microsoft 365), `adobe` (zstd, to unpack AdobeWine), `archives` (7-Zip), `performance` (GameMode, MangoHud) or `controllers` (the SDL3 library, for big screen); without it, all of them. Returns `{"distro", "family", "missing": [{"tool", "package", "purpose"}], "unavailable": [{"tool", "purpose"}], "install": [argv...], "restart"}`. `unavailable` lists what's missing with no package on this distribution (on Debian and Ubuntu, a name `apt-cache` doesn't know). `family` is `arch`, `debian`, `fedora`, `suse`, `ostree` (Bazzite, Silverblue: layered with rpm-ostree, so `restart` is true), `steamos` or `unknown`. `install` is the command that installs the missing packages, for a client to run as root (`pkexec`), and empty when nothing's missing or Mira has no way to install them (SteamOS, unknown). Running winetricks or installing a launcher that uses it fails with `missing_packages` and fix `{"kind": "packages", "target": "winetricks"}` until they're there.

### `GET /v1/gamemode/status`
`{"installed": true, "daemon_running": false}`. `installed` means `gamemoded` or `gamemoderun` is on `PATH`; `daemon_running` means GameMode owns its D-Bus name. With `launch.gamemode` on, an unreachable daemon is logged and the game still launches.

## Metadata

Store info is cached in `cache.db` in the config directory, and art as files under `artwork/<id>/`, with `cache.db` pointing at each slot's file. All of it can be fetched again, so a damaged `cache.db` is started over. Sources:

- **Steam games**: Steam's store API, review summary and CDN art (`cover`, `hero`), plus the ProtonDB tier. No key needed.
- **GOG, itch and Amazon**: cover and hero from GOG Galaxy's games database, with nile's cached art as a fallback for Amazon.
- **Everything else**: [SteamGridDB](https://www.steamgriddb.com) by name (`cover`, `hero`, `logo`, `icon`) when `steamgriddb.api_key` is set. Without a key, `metadata.steam_art_by_name` borrows art from a Steam game of the same name. If nothing is found, the fetch fails with `no_steamgriddb_key`.

With a key, SteamGridDB also adds alternates for every game in `art_candidates`, without replacing store art. `metadata.steam_by_name` (on by default) looks up a ProtonDB tier by name for non-Steam games, and Steam's reviews and tags for a Steam game of exactly the same name. A game's `metadata.steam_appid` replaces the name match.

Art is shrunk as it's saved to fit its slot (`metadata.art_size`: `auto`, `compact`, `high` or `original`): a cover within 900x1350, a hero within 1920 wide, a logo within 800 and an icon within 256, or about two thirds of that when compact. `auto` is high, or original on a 4K or larger screen, and original is never shrunk. It's saved as JPEG, or PNG when it has transparency. A JPEG that already fits is kept as downloaded.

New games are fetched when first added, through a queue of three workers. Tracked games go before store titles. `metadata.enabled` turns automatic fetching off. Details (store info, reviews, ProtonDB tier) older than `metadata.refresh_days` are fetched again on start, without the art. `details_fetched` is when they last were.

The routes below that take a game's `{id}` (metadata, artwork, candidates, thumbs, matches and match) also take a store title that isn't installed, by the id it gets once installed (`<source>-<ref>`), as long as that source's stored list (`GET /v1/library`) has it; `404 game_not_found` otherwise. A title's art is picked, uploaded and removed like a game's. With no SteamGridDB match recorded yet (its cover came from its store), its candidates are looked up by its title.

### `GET /v1/games/{id}/metadata`
The cached JSON: `source`, `fetched_at`, `details_fetched`, and whichever of `steam`, `steam_reviews`, `epic`, `protondb`, `artwork` (the cover), `hero`, `logo` and `icon` were found. Art entries look like `{"file", "content_type", "source", "candidate_id"?, "chosen"?}`; `chosen` marks a slot the user picked. `art_candidates` maps each slot to `[{"id", "url", "thumb", "width", "height", "style", "nsfw"}]`; adult art is only listed with `steamgriddb.nsfw` on and is never picked by default. A refetch keeps the art SteamGridDB's top result gave a slot while the match (`steamgriddb_id`) stays the same, since that order changes as art is added and voted on. `steam` includes `controller_support` (`"full"`, `"partial"` or `""`) from Steam's store. `404` when nothing is cached.

### `GET /v1/games/{id}/artwork?type=`
The cached image for a slot (`cover` by default). `404` if that slot isn't cached.

### `POST /v1/games/{id}/artwork?type=`
Body `{"candidate_id": <id>}`. A job (kind `artwork`) that switches a slot to a cached candidate. Only candidate ids are accepted, never URLs. A metadata refresh keeps the pick; picking again is the only way to change it. With `steam.sync_art` on, a Steam game's picks (this, an upload or a removal) also go into Steam's `userdata/<id>/config/grid/` before the event. Events: `game.artwork_selected`/`artwork_select_failed`.

### `PUT /v1/games/{id}/artwork?type=`
The body is a PNG or JPEG of up to 32 MB that becomes the slot's image (`cover`, `hero`, `logo` or `icon`), fitted like downloaded art and kept through refreshes like a pick, with `"source": "upload"`. A job (kind `artwork`) with the same events as picking a candidate. Fails with `missing_image` for an empty body, `unsupported_image` for anything else or an image that can't be read, `413 image_too_large` over the limit.

### `DELETE /v1/games/{id}/artwork?type=`
Leaves the slot with no image, so clients draw their placeholder: the slot becomes `{"source": "none", "chosen": true}` with no file and is kept through refreshes like a pick, and a store title without a cover isn't fetched again for it. Picking or uploading again replaces it. A job (kind `artwork`) with the same events as picking a candidate.

### `POST /v1/games/{id}/artwork/candidates?type=&page=&request=[&style=][&adult=1]`
Fetches one page (50) of SteamGridDB art for a slot, starting at page 0, and adds it to `art_candidates`. `style` narrows it to one of SteamGridDB's styles for the slot (covers: `alternate`, `blurred`, `white_logo`, `material`, `no_logo`; heroes: `alternate`, `blurred`, `material`; logos: `official`, `white`, `black`, `custom`; icons: `official`, `custom`), and `adult=1` to adult art, which needs `steamgriddb.nsfw`. Event: `game.artwork_candidates_ready` with `{id, type, page, request, total, candidates}`, where `total` counts what matches; unfiltered page 0 adds the whole list's counts, `styles` (`{style: count}`, styles with none left out) and `adult`. On failure `code` and `error` (`no_steamgriddb_key`, `no_steamgriddb_match`, `steamgriddb_unreachable`, `invalid_style`, `adult_art_off`). `request` is echoed back so a caller can match its answer.

### `POST /v1/games/{id}/artwork/thumbs?type=`
Body `{"candidate_ids": [...]}`, 1 to 64 ids. Caches a preview for each, in parallel. Event: `game.artwork_thumbs_ready` with `{id, type, ready, failed}` and `error` if the whole batch failed.

### `GET /v1/games/{id}/artwork/thumb?type=&candidate_id=`
One cached preview. `404 thumb_not_cached` until fetched.

### `DELETE /v1/artwork/thumbs`
Deletes all cached previews (`204`). The GUI calls it on quit, and `mirad` clears them on start and stop.

### `DELETE /v1/games/{id}/artwork/thumbs`
Deletes one game's cached previews (`204`). The GUI calls it as the game's settings close.

### `POST /v1/games/{id}/metadata/refresh?announce=`
Fetches one game again, even with `metadata.enabled` off. Events: `game.metadata_ready`/`metadata_failed` with `code`, `error`, and the error's `hint` and `fix` when it has them. With `announce=1`, a failure also publishes a `notification`, except for `no_steamgriddb_key`.

### `GET /v1/games/{id}/metadata/matches[?q=]`
SteamGridDB's matches for the name (or `q`): `{query, chosen, matches: [{id, name, release_date?}]}`. `chosen` is 0 when the top match is in use.

### `POST /v1/games/{id}/metadata/wrong-match`
Moves to the next SteamGridDB match, saves it as `metadata.steamgriddb_id` and fetches again. `202 {status, match}`, or `409 no_more_matches`.

### `POST /v1/games/{id}/metadata/match`
Body `{"steamgriddb_id": N}`. Uses that SteamGridDB game from now on; `0` goes back to the top match. A game is fetched again (`202`). A store title has no overrides, so its record keeps the match as `steamgriddb_id`; its SteamGridDB candidates are dropped, its current art stays, and `game.metadata_ready` follows at once (`200 {status: "matched"}`).

### `POST /v1/games/metadata/refresh`
Body `{"ids": [...]}`. Fetches each of these games again, like `POST /v1/games/{id}/metadata/refresh` without `announce`. Unknown ids are skipped. A job (see [Jobs](#jobs)) of kind `metadata`: `job.progress` counts games as their fetches end, and the result is `{"refreshed", "failed"}`. Each game still sends its own `game.metadata_ready`/`metadata_failed`.

### `POST /v1/games/metadata/refresh-missing`
The same, for every game without a cover.

## Achievements

A game's achievements as its store last listed them, cached in `cache.db` and refreshed on request, after a tracked session of the game ends (for Steam games only with `steam.track_process`), never on a timer. A game whose store listed none within the last week isn't asked again after playing. `achievements.enabled` (on by default) turns refreshing off; what's cached still answers. Sources:

- **Steam games** (`runner_ref` `steam:<appid>`): every definition from Steam's `appcache/stats/UserGameStatsSchema_<appid>.bin` and the account's progress from `UserGameStats_<account>_<appid>.bin`, which Steam writes for games it has run on this computer. The account is `steam.steamid64`, else the one Steam signed in last. Without those files, Steam's Web API when `steam.web_api_key` and `steam.steamid64` are set, else `no_local_achievements` with a fix for the key. Rarity comes from Steam's global percentages, which need no key. Unlocking stays with the Steam client.
- **GOG games**: GOG Galaxy's gameplay API (`gameplay.gog.com/clients/<product id>/users/<user id>/achievements`) with the account gogdl signed in, rarity included. Undocumented, so a change on GOG's side fails the refresh rather than anything else. Signed out, the error's fix is `{"kind": "source", "target": "gog", "step": "login"}`.
- **Epic games**: `legendary achievements <app> --json` (Legendary 0.21.1 or newer; an older one fails with `legendary_too_old` and a fix to set the source up again), in Epic's own order from Legendary's metadata cache, with rarity and XP. A game without Epic achievements is an empty list.

Text is in the system language (`LC_ALL`, `LC_MESSAGES`, `LANG`), falling back to English.

### `GET /v1/games/{id}/achievements`
`{source, fetched_at, total, unlocked, items: [{key, name, description, hidden, unlocked, unlocked_at, rarity, xp, icon}]}`, items in the store's order. `unlocked_at` is unix seconds (0 while locked or when the store doesn't say), `rarity` the percent of players who have it (-1 when unknown), `xp` Epic's points (0 elsewhere). A `hidden` achievement keeps its name and description: a client masks it until unlocked. `icon` is a version for the icon endpoint, empty when the achievement has no icon. `404 no_achievements` before the first refresh, `400 no_achievements_source` for a game whose source has none. While `achievements.enabled` is off, this route, the refresh and the icon answer `400 achievements_off`; the cached lists are kept for when it's turned on again.

### `POST /v1/games/{id}/achievements/refresh`
Asks the game's store again. A job (kind `achievements`) whose result is `{id, total, unlocked, changed}`. Event: `game.achievements_ready {id, total, unlocked}` when the list changed.

### `GET /v1/games/{id}/achievements/icon?key=&state=locked|unlocked`
One achievement's picture (`unlocked` by default; a store with one picture uses it for both), downloaded the first time it's asked for, a few at a time, fitted within 128x128 and kept under `artwork/<id>/achievements/`. Icons no longer listed are deleted on refresh, and all of them with the game's other art. `404 achievement_not_found`, `404 icon_not_found`, `502 icon_download_failed`.

## Saves

Where each game keeps its saves comes from a list of save locations, downloaded from `saves.manifest_url` and indexed in `cache.db`. Only the list's data is used (by default the [ludusavi-manifest](https://github.com/mtkennerly/ludusavi-manifest) project's, MIT); finding, backing up and syncing the files is Mira's own. It's fetched the first time something needs it, then at most once a day when something does, never on a timer; after a failed fetch, not again for an hour. An update sends the last ETag and asks for a compressed transfer, and a download identical to the indexed one isn't indexed again. Entries that list no save files and no registry keys are left out, so a game whose entry has only install or launch info has none.

A game's entry is found by the first of: its `saves.manifest_name` (an entry no longer in the list is skipped), its Steam app id (`runner_ref` `steam:<appid>`, else `metadata.steam_appid`, also matching an entry's extra Steam ids), its GOG product id, its Lutris slug (as the entry's Lutris id, then read as a name), then its exact name. Names compare in lower case, without ™ ® © or apostrophes, other punctuation read as spaces. Aliases lead to the entry they name. There's no matching by similarity.

### `GET /v1/games/{id}/saves`
`{enabled, status, entry, match, locations: [{pattern, path, files, bytes, skipped, extra, problem?}], snapshots: [{id, taken_at, reason, files, bytes, pinned}], last_error?, cloud, doing, folders: {install, prefix}}`, answered from the index and the disk without the network (it starts a fetch of the list when that's due). `entry` is the list's entry for the game and `match` the rule that found it (`setting`, `steam`, `gog`, `lutris` or `name`), both null when none did. `folders` are the game's install folder and prefix (the folder holding `drive_c`, empty for a native game), where a screen can start a folder picker for `saves.extra_paths`. `status`:

- `kept`: Mira backs up its saves and has at least one backup.
- `ready`: save files were found.
- `no_files`: the game has save locations, but nothing is there yet.
- `no_entry`: no save locations are known for it on this system: no entry, or one whose locations are all for another system or store. `saves.extra_paths` adds the game's own.
- `registry_only`: its entry keeps saves only in the Windows registry, which Mira doesn't back up: restoring a prefix's `user.reg` would also roll back every other setting in the prefix.
- `steam_cloud`: a Steam game the Steam client starts (`steam.launch_mode` `steam`) whose saves Steam Cloud keeps, while `saves.skip_cloud` is on.
- `off`: `saves.enabled` is off for the game (`enabled` false).

`locations` are the entry's save locations that apply to the game, then its `saves.extra_paths`: those whose `when` allows its system (Wine and Proton count as `windows`) and, for a Steam, GOG or Epic game, its store; a location tagged only `config` (settings, not saves) is left out. `path` is the location with the game's folders filled in, and `extra` marks one from `saves.extra_paths`. The same folder named twice (by the list and the store, say) is listed once. A location that would take a whole prefix, home or user folder or one of their shared folders (AppData, Documents, `~/.local/share`) whole has `problem: "too_broad"` and isn't read; one using a folder the game doesn't have (a Windows folder for a native game) has `problem: "unknown_placeholder"`. Placeholders are the list's (`<base>`, `<prefix>` (Mira's own: the folder holding `drive_c`), `<root>`, `<home>`, `<winAppData>`, `<winLocalAppData>`, `<winLocalAppDataLow>`, `<winDocuments>`, `<winPublic>`, `<winProgramData>`, `<winDir>`, `<xdgData>`, `<xdgConfig>`, `<storeUserId>`, `<storeGameId>`, `<osUserName>`, `<game>`), GOG's (`<?INSTALL?>`, `<?APPLICATION_DATA_ROAMING?>`, `<?APPLICATION_DATA_LOCAL?>`, `<?APPLICATION_DATA_LOCAL_LOW?>`, `<?DOCUMENTS?>`, `<?SAVED_GAMES?>`) and Epic's (`{installdir}`, `{appdata}`, `{roaming}`, `{locallow}`, `{userdir}`, `{usersavedgames}`). In a prefix they're read in the Windows user folder (`steamuser` once Proton made it, else `$USER`), case ignored, Wine's older "Application Data" and "My Documents" tried too; a Steam game's prefix is its `compatdata/<appid>/pfx` and its `<root>` both its library folder and Steam's own. A folder that matches brings every file under it. Symlinks inside a save folder aren't followed and are listed in `skipped`; Wine's `dosdevices` is never entered. Locations are listed whatever the status.

`snapshots` are the game's backups, newest first: `id` names it (the UTC time it was taken, such as `20261021T143005Z`), `taken_at` is unix seconds, `reason` is `first` (before the game's first launch with Mira keeping its saves), `after_play`, `manual`, `before_restore` or `before_cloud_download`, and `files` and `bytes` are what it holds; a `pinned` one is kept whatever `saves.keep` says. `last_error` (`{code, message, hint?, at}`) is why the last backup after play failed, until one succeeds. `doing` is what's being done with the game's saves now (`backing_up`, `restoring`, `syncing`), or empty; `game.saves_activity` says when it changes.

An absolute path set in a game's `saves.extra_paths` (`PATCH /v1/games/{id}/config`) is stored written against the game's folders when it lies in one, so it follows the game when it or its prefix moves: `<base>/...` inside its install folder, `<home>/...` inside a Windows game's prefix user folder or a native game's home folder, `<prefix>/...` elsewhere in its prefix, the deepest that fits. Paths are glob patterns: a folder name with `*`, `?` or `[` in it is written `[*]`, `[?]`, `[[]`.

Backups go to `saves.backup_root`, or a `saves` folder in the first of `mira_folders` when that's empty, one folder per game id, so a removed game added back under its id finds them. Each is `<id>/files/<root>/<path>` plus a `snapshot.json`, where `root` is one of the game's folders (`install`, `prefix`, `prefix_user`, `steam_library`, `steam`, `home`) and `path` is inside it, so a backup still restores to the right place after the game or its prefix moved. Copies keep each file's modified time; a file unchanged since the previous backup is a hard link to its copy there, so ten backups of a save set where one file changes take about one copy plus the changes. A backup is taken after each session of a game whose saves Mira keeps, unless nothing changed (the same files, sizes and modified times); it isn't taken while the game runs, moves or is deleted. The newest `saves.keep` backups taken after play or by hand are kept, plus pinned ones (not counted), plus the copies taken before a restore or a cloud download that are newer than the oldest of them. A game whose saves are over `saves.max_mb` isn't backed up (`saves_too_large`, naming the location holding the most); one whose disk is short of room fails with `saves_disk_full`. Each publishes `game.saves_changed {id}`, as does a failure after play.

### `POST /v1/games/{id}/saves/backup`
Backs up the game's saves now, whatever `saves.enabled` and `saves.skip_cloud` say. A [job](#jobs) (kind `saves`, target the game) whose result is `{snapshot, taken}`: the new backup, or with `taken` false the newest when nothing changed. Fails with `saves_not_found` when no save files were found, `saves_too_large`, `saves_disk_full`, `saves_failed`, or `game_running`/`game_busy`/`saves_busy`.

### `POST /v1/games/{id}/saves/restore`
`{"snapshot": "<id>"}` puts that backup's files back where the game keeps them now, with their modified times, by their folders (`root`), so they land in the game's current folder or prefix and its current Windows user. Files the backup doesn't hold are left alone. What's there is backed up first (reason `before_restore`), so a restore can be undone by restoring that one. A [job](#jobs) (kind `saves`, target the game) whose result is `{restored, files, undo}`: `undo` is the backup holding what was replaced, the newest one when nothing had changed since, or null when there were no saves. `404 snapshot_not_found` for an unknown backup. Fails with `saves_unplaceable` before writing anything when the game no longer has a folder the backup needs (a prefix, for a game now native), with the backup's errors when the first copy can't be taken, or with `game_running`/`game_busy`/`saves_busy`. Publishes `game.saves_changed`.

### `POST /v1/games/{id}/saves/folders`
`{"add": "<path>"}` adds one file or folder to the game's `saves.extra_paths`, matching only itself (`*`, `?` and `[` in its names bracketed) and written against the game's folders as `PATCH /v1/games/{id}/config` writes them; `{"remove": "<pattern>"}` takes one out as it's listed (a location's `pattern`), clearing the game's own value when none are left. The list is changed where it's stored, so edits made one after another from several clients all land. `{extra_paths}`, the game's list now; `400 invalid_body` without exactly one of the two, or `404 game_not_found`. Publishes `game.updated`.

### `PATCH /v1/games/{id}/saves/{snapshot}`
`{"pinned": true | false}` pins a backup, so it's kept however many newer ones are taken and doesn't count toward `saves.keep`, or unpins it. `{}`, `400 invalid_body`, or `404 snapshot_not_found`. Publishes `game.saves_changed`.

### `DELETE /v1/games/{id}/saves/{snapshot}`
Deletes one backup, pinned or not, also while the game runs. `{}`, or `404 snapshot_not_found`. Publishes `game.saves_changed`.

#### Cloud saves

GOG and Epic games' saves are synced with the store's cloud by Mira itself, as Galaxy and the Epic launcher do. Where they go comes from the store (GOG's remote config for the game's client, which can name several locations; Epic's cloud save folder from `legendary info`), cached in `cache.db` for a week and read by the same resolver as the list's locations, so they land in the game's own prefix; their folders are backed up with the rest of its saves. A location Mira can't place (a placeholder it doesn't know, a wildcard) makes the sync fail with `cloud_unsupported` rather than guess. GOG keeps cloud saves only for its Windows games here.

Mira decides each location's direction from what it recorded when they were last in sync (`mira.db`, kept on a plain removal, dropped with `delete_metadata`): the local files' paths, sizes and modified times then, and the cloud's newest time then (GOG's listing; Epic's newest upload). Local changed and the cloud not: upload; the cloud changed and local not: download; both: a conflict, and nothing moves until one side is picked. On a first sync, an empty side takes the other's; when both have saves, GOG's checksums or (for Epic, which gives none) newest times within a minute of each other count as in sync, and anything else is a conflict. Only then is gogdl or legendary run, always with a forced direction and Mira's folder (`--save-path`), so neither tool's own decision is used; gogdl's "failed" log lines after an exit 0 count as a failure. Before anything local is overwritten, a `before_cloud_download` backup is taken.

Saves are synced before each launch (see [launch](#post-v1gamesidlaunch)) and again after play, on the saves lane after the after-play backup. A conflict found after play (the cloud changed on another computer meanwhile) overwrites neither side and publishes a persistent `notification` with the error's `code`, `hint` and `fix`. Each sync publishes `game.saves_activity {id, doing: "syncing"}` as it starts, then `game.saves_changed` and `game.saves_activity` with `doing` empty when it ends; after play, the backup and the sync are one stretch of work.

`cloud` in `GET /v1/games/{id}/saves` is null for a game whose store has no cloud saves Mira syncs, else `{store, state, synced_at, checked_at, local_at, cloud_at, files, bytes, locations: [{name, path, state}], reason?}`, from what's stored and the disk, never the network. `state` is `synced`, `local_newer`, `cloud_newer`, `conflict`, `unknown` (not checked yet), `unsupported` (with `reason`) or `off` (`saves.cloud` is off for the game); the whole game's is its most pressing location's. A location found in sync or behind the cloud at its last check reads `local_newer` or `conflict` once its files changed since. `local_at` is this computer's newest save file, `cloud_at` the cloud's newest upload at the last check, and `files` and `bytes` what this computer holds in the cloud's locations, so a conflict can say what each side has. Times are unix seconds, 0 for never or none.

### `POST /v1/games/{id}/saves/cloud`
`{"action": "check" | "sync" | "download" | "upload"}`, whatever `saves.cloud` says. `check` compares and stores the states; `sync` also moves each location the way it's behind, failing with `saves_conflict` (with a `fix` to the game's saves and nothing moved) when one changed on both sides; `download` and `upload` replace one side with the other, for picking a side of a conflict (a side with no saves is left as it is). A [job](#jobs) (kind `saves`, target the game) whose result is `{state, locations: [{name, path, state, moved?}]}`, `moved` being `download` or `upload`. `409 cloud_unsupported` for a game of another store; the job fails with `cloud_unsupported` when the store keeps none for the game, `not_authenticated` (fix: sign in), `cloud_sync_failed`, `gog_api_error`, `game_running` or `game_busy`. Publishes `game.saves_changed`. The game's GOG client secret and tokens never appear in a reply, event or log.

### `GET /v1/saves/manifest`
`{url, games, fetched_at, checked_at}`: the list's address, how many games the index holds (0 before the first download, or after `saves.manifest_url` changed until it's fetched from there), when it was last downloaded and when last checked (unix seconds, 0 for never).

### `POST /v1/saves/manifest/update`
Checks for a newer list now. A job (kind `saves`, target `saves-manifest`) whose result is `{changed, games}`, publishing `saves.manifest_changed` when `changed`; while one runs, answers with its id. A list that doesn't parse fails with `manifest_invalid` and the index stays as it was; reads go on answering from the old index while a new one is built.

### `GET /v1/saves/entries?q=`
`{entries: [{name, alias_of?}]}`: up to 50 entries whose name contains `q`, names starting with it first, then shorter names first. `alias_of` is the entry an alias leads to. Starts a fetch when the list is due, answering from the index there is meanwhile (empty before the first download).

## Events

### `GET /v1/events`
Server-Sent Events:

```
id: 42
event: game.updated
data: {"id":"celeste","name":"Celeste", ...}
```

A new connection (no `Last-Event-ID`) first gets the buffered events replayed, then a `stream.live` event with no id: everything after it is new. Show replayed events as state, and announce only what arrives after `stream.live`. Reconnect with `Last-Event-ID` to replay what was missed; a resumed connection gets no `stream.live`. The buffer holds the last 500 events in memory. A client that falls further behind than that, or resumes from an id the buffer no longer holds, gets a `stream.gap {"missed": n}` event with no id just before the next event it can have, and should list what it shows again. Ids start from the clock, so they keep increasing across a daemon restart. Each stream holds one of mirad's request threads, so at most 16 are open at once; another gets `503 too_many_streams`.

| Event | Payload |
|---|---|
| `game.added` | The game, plus `open_config` from the `open_config_on_add` setting and, for a scanned folder, `auto_install`: whether the scan runs its installer on its own. |
| `game.updated` | The game. |
| `games.updated` | `{games}`: every game a `PATCH /v1/games` changed, or, at startup, every game that isn't running (a client open across a restart may still show some as running). |
| `game.removed` | `{id}`. |
| `games.removed` | `{ids}`, from `POST /v1/games/delete`, a store import that found games uninstalled, or Steam finishing an uninstall Mira handed it. |
| `config.changed` | `{keys, frontend}` after `PATCH /v1/config`, a reset, or a hand edit of `settings.toml`: the dotted settings keys that changed (names only, never values) and the whole `frontend` table, so a client applies a change made elsewhere. |
| `game.state` | The game plus `state` (`running`, `exited`, `crashed`, `idle`) and, after an exit, `exit_code`, `signal`, `played_seconds` and `error`. `crashed` means a crash signal (or a shell's 128 + one), exit code 126/127 or a program Wine couldn't load, or Wine's "Unhandled ..." report in the log followed by a non-zero exit; a plain non-zero exit is `exited`, and so is anything after a stop. A crash adds `code` (`crashed`, `killed` or `start_failed`), a plain-language `error` that is also the game's `last_error`, a `hint` and a `fix` that opens the game's log. |
| `game.install_detected` | `{id, install_path, exe_path}`, after a launched Windows game exits and its prefix gained a program folder, i.e. the "game" was an installer. `exe_path` is relative to `install_path`, empty when no program was found. Adopt it with `finish-install`. |
| `game.installer_leftover` | `{id, installer_dir, bytes}`, after an install or `finish-install` left the game somewhere other than its installer's folder, which is still on disk inside a library root (never a disc or a folder outside them). Delete it with `DELETE /v1/games/{id}/installer`. |
| `game.launched` | `{id, via, tracked}` for launches handed to Steam or a store launcher. |
| `game.install.*` | See `POST /v1/games/{id}/install`. |
| `game.metadata_ready`, `game.metadata_failed` | See metadata refresh. |
| `game.artwork_*` | See the artwork endpoints. |
| `game.achievements_ready` | `{id, total, unlocked}` after an [achievements](#achievements) refresh changed the list. |
| `game.saves_changed` | `{id}` when a game's [save](#saves) backups or cloud state changed, or a backup after play failed or succeeded again. |
| `saves.manifest_changed` | No payload: a newer save locations list is in use, so any game's `GET /v1/games/{id}/saves` may answer differently. |
| `tricks.*` | See `POST /v1/games/{id}/tricks`. |
| `library.install.*` | `{source, ref, update}`; `progress` adds `progress`, `eta` and `bps`. |
| `library.artwork_ready`, `library.artwork_failed` | `{source, ref}`, plus the error fields on failure. |
| `library.catalog_checking`, `library.catalog_checked` | `{source}` while `GET /v1/library` re-checks a store's owned titles; `checked` adds `changed`. |
| `library.hidden_changed` | `{source, refs, hidden}` after `POST /v1/library/hidden`. |
| `hidden.changed` | `{password, locked}` when the [hidden games](#hidden-games) password is set or removed, or they lock or unlock. |
| `concealed` | No payload: a buffered event about a hidden game, blanked when hidden games locked. |
| `job.started`, `job.progress`, `job.finished`, `job.failed` | See [Jobs](#jobs). |
| `runners.download.*`, `runners.updated`, `runners.removed` | See the runner endpoints. |
| `umu.setup.*`, `winetricks.setup.*` | Tool installs. |
| `launcher.install.*` | See `POST /v1/launchers/{id}/install`. |
| `game.saves_activity` | `{id, doing}` when what's being done with a game's saves changes: `backing_up`, `restoring` or `syncing` as it starts, empty when it's done (as `doing` in `GET /v1/games/{id}/saves`). |
| `notification` | A message for the user, with its level. `persistent: true` marks one that should stay until dismissed; a game's carries its `id`, and one from an error carries `code`, `error`, `hint` and `fix`. |

`mira watch` in `src/cli/main.cpp` is a minimal client.
