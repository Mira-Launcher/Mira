# API reference

REST over HTTP/1.1 on a Unix socket, never TCP. The default socket is `$XDG_RUNTIME_DIR/mira/mirad.sock`; the `socket_path` setting or `mirad --socket <path>` changes it, and `$MIRA_SOCKET` points the GUI and CLI elsewhere. Routes are registered in `Server::RegisterRoutes` in `src/api/Server.cpp`, and the code wins if this file disagrees.

Bodies are JSON. Errors share one envelope:

```json
{ "error": { "code": "invalid_setting", "message": "scan.debounce_ms: must be between 0 and 600000" } }
```

`code` is stable and meant for code; `message` is meant for people and says what went wrong; it is never empty (a failure with nothing to say gets its code in words). Two optional fields say what to do about it:

- `hint`: one sentence for the user, worded for any client (no CLI commands, no GUI paths).
- `fix`: where the fix is, for a client to turn into a button or a command. `{"kind": "setting", "target": "<dotted key>"}`, `{"kind": "runners", "target": ""}` (install a runner) or `"target": "winetricks"`, `{"kind": "source", "target": "<source id>", "step": "setup" | "login" | "install"}`, or `{"kind": "game", "target": "<game id>", "step": "exe" | "data_dir" | "log" | "install"}` (`install`: run its installer with the window shown).

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

Events: `job.started {id, kind, target, label}`, `job.progress {id, done, total, message}` where the work has steps, then `job.finished {id, kind, target, result}` or `job.failed {id, kind, target, error}`. `result` is what the endpoint describes as its reply; `error` is the usual `{code, message, hint?, fix?}`, or `internal_error` for a bug in mirad. `kind` is `scan`, `import`, `relocate`, `delete`, `remove_source`, `metadata`, `install`, `update`, `setup`, `download`, `runner`, `tricks` or `artwork`; `target` is the source, game, store, tool or runner it's about, or empty. Installs, runner downloads and tool setups run a few at a time, and winetricks verbs one at a time.

### `GET /v1/jobs/{id}`
`{id, kind, target, label, state, progress?, result?, error?}` with `state` `running`, `finished` or `failed`. The last 100 jobs are kept, never dropping one still running; an older one is `404 job_not_found`.

### `POST /v1/jobs/{id}/cancel`
Stops a running job: the programs it runs (a store tool, an installer and the Wine processes in its prefix, a download) are killed, and it ends as `job.failed` with code `cancelled`. Its own failure event (`library.install.failed`, `game.install.failed`, `launcher.install.failed`, `runners.download.failed`, `<tool>.setup.failed`) carries `code: "cancelled"` too. Store tools keep what they downloaded, so installing again resumes. `{"status": "cancelling"}`; `404 job_not_found`, or `409 not_running` once it has ended.

## Health

### `GET /v1/health`
`{"status": "ok", "api": 1}`. `api` is an integer bumped on a breaking change; the GUI and CLI compare it with their own and report a daemon that doesn't match.

## Settings

Stored in `settings.toml`. Every key is declared once in `src/config/Schema.cpp`.

### `GET /v1/config`
Every setting at its current value, plus the `frontend` table, which the backend stores without interpreting: `frontend.toml`, with the window state the GUI sets as it's used (sizes, sort, last filter, `onboarded`, `source_imported_at`) merged in from `mira.db`.

### `GET /v1/config/schema`
Every setting in display order:

```json
[{ "key": "scan.debounce_ms", "label": "Scan delay (ms)", "type": "an integer",
   "default": 3000, "scope": "global", "category": "Scanning", "group": 0,
   "group_label": "Scanning",
   "doc": "How long a new folder must stay unchanged before Mira scans it.",
   "minimum": 0, "maximum": 600000 }]
```

`category` is the settings section. Within it, settings sharing a `group` number belong together under the title `group_label`. Labels and group titles are in sentence case. `scope` is `per_game` when a game can override the setting and `game_only` when it only exists per game. Optional fields, present only when they apply: `game_doc` (help text for a game's own settings, used instead of `doc`), `one_of` (enum values), `minimum`/`maximum`, `is_secret` (mask the value), `is_runner_ref` (offer a runner picker), `link` (a web page where the user gets the value, e.g. an API key page), `keywords` (extra search terms such as abbreviations, space-separated), `group_collapsed` (the group holds rarely changed values, best shown folded), `group_resettable` (the group's values make sense to put back to their defaults together, so a screen can offer one reset for the group), `path` (`"folder"` or `"file"` when the value, or each item of an array, is a path on this computer, so a screen can offer a picker) and `source` (the source the setting belongs to, e.g. `"steam"` or `"ubisoft"`, so a screen can gather one source's settings together).

### `PATCH /v1/config`
Sets any subset of settings, nested like `GET /v1/config`, plus an optional `frontend` key, merged into `frontend.toml` as a JSON merge patch (a `null` value deletes that key). The whole patch is validated first; one bad value means nothing is applied.

### `POST /v1/config/reset[?key=<dotted.key>]`
Resets one key, or everything when `key` is left out.

## Games

Stored in the library database, `mira.db`. A game's `id` is a readable slug such as `celeste`, or `celeste-2` on a clash.

### `GET /v1/games[?status=][&tag=][&include_hidden=true]`
Lists games, optionally filtered by `status` (`setting_up`, `ready`, `broken`, `missing`, `needs_install`) and by tag. Games tagged `hidden` are left out unless `tag` is given, so `?tag=hidden` lists only those, or `include_hidden=true` is, which lists them alongside the rest. With `scan.tag_by_root` on, games a scan finds in a library root are tagged with its name; games added by hand, installed into a prefix or imported from a store aren't.

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

- `art` lists the art slots Mira has an image cached for, each with a version: `{"cover": "18f3a…-2c41", "hero": "…"}`. A slot left out has no image, so `GET /v1/games/{id}/artwork` for it would 404. The version changes whenever the slot's image does, so a client can keep its copy until then. Every game record has it, and so do `game.metadata_ready`, `game.metadata_failed` and `game.artwork_selected`.

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
Changes any of `name`, `exe_path`, `args` (one command line: arguments are split on spaces, and quotes keep one together, as in `--save "C:\My Games"`), `working_dir`, `runner_ref`, `data_dir`, `platform` (`windows` or `native`; anything else is `400 invalid_body`; a game turned `windows` without a `data_dir` gets one under `prefix_root`, and a ready one becomes `setting_up` while a `provision` [job](#jobs) sets up its prefix, then `ready` or `broken` with a `game.updated`; a game turned `native` keeps its `data_dir`, so turning it back reuses the prefix), `runner_config` (merged), `env` (merged, `null` removes a key), `tags` (replaced) and `folder_tag` (in a library folder sorted by tag, a change to either can move the game, see [Folders by tag](#folders-by-tag)). An `exe_path` given relative but outside the game's folder (`../Applications/Eden.AppImage`) is stored absolute, here and in `POST /v1/games/manual`, so it survives a move. Any change marks the game `reviewed`; `{"reviewed": true}` confirms a game without changing anything else. Overrides go through `/config` below. Publishes `game.updated`.

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

`install_path` and `exe_path` (relative to `install_path`) are required. `name` defaults to the cleaned folder name, or the file's own name for an AppImage, and `platform` to `windows` for `.exe`, else `native`; a `platform` other than those two is `400 invalid_body`. `is_installer` stores the game `needs_install`. A Windows game is returned `setting_up` while a `provision` [job](#jobs) sets up its prefix, then becomes `ready` or `broken` with a `game.updated`. Adding the same `install_path` and `exe_path` again updates that game, and so does another program in its folder. An AppImage is a game of its own, so each AppImage in one folder is a separate game. Returns the game and publishes `game.added` or `game.updated`.

### `DELETE /v1/games/{id}[?delete_files=true][&delete_prefix=true][&delete_metadata=true][&purge=true]`
Removes the game from the library. Nothing on disk is touched unless asked:

- `delete_files` removes `install_path`.
- `delete_prefix` removes `data_dir`.
- `delete_metadata` removes cached metadata and art.
- `purge` does all three.

Files are only deleted when they resolve inside a library root, a store's install root (`epic.`, `gog.`, `itch.`, `amazon.install_root`) or the game's own prefix, and prefixes inside `prefix_root`; never for a `desktop-entry` game, whose files belong to another app. For Epic, Amazon and itch.io games, `delete_files` uninstalls through `legendary`, `nile` or butler so the store's records stay correct. A game run from an AppImage loses just the AppImage; for any other game, a folder that also holds another game or a library root isn't deleted (`shared_folder`). A program the game only runs (see relocate below) is never deleted. Publishes `game.removed`.

With `delete_files`, `delete_prefix` or `purge` it is a [job](#jobs) (deleting can take minutes) whose result is `{}`, failing with the error that kept the game; otherwise it answers `{}` straight away.

### `POST /v1/games/delete`
The same for many games: `{"ids": [...], "delete_files"?, "delete_prefix"?, "delete_metadata"?, "purge"?}`, flags as above. A folder shared only by games removed together is deleted; one another game still uses isn't (`shared_folder`). A game whose files or prefix can't be deleted stays in the library. Unknown ids are skipped. A [job](#jobs) whose result is `{"removed": [ids], "failed": [{"id", "error": {...}}]}`, with each error shaped like the error envelope, and publishes one `games.removed` event with the removed `ids`.

### `GET /v1/games/{id}/config`
Every setting as it resolves for this game, with the layer it came from:

```json
{ "launch.gamemode": { "value": true, "layer": "game", "overridable": true },
  "library_roots": { "value": ["~/Mira/Games", "~/Mira/Applications"], "layer": "default", "overridable": false } }
```

### `PATCH /v1/games/{id}/config`
Sets or removes (`null`) this game's overrides as a flat `{"dotted.key": value}` body. Only `per_game` keys are accepted, and nothing is applied if any key fails.

### `POST /v1/games/{id}/launch`
Resolves `runner_ref` (or the platform's `default_runner.*`) and starts the game. A Microsoft 365 app takes an optional body `{"files": ["/absolute/path", ...]}` and opens those documents, also while it's already running (`400 files_unsupported` for anything else); its desktop entry declares the file types it opens and passes them on. `409 needs_install` or `409 not_ready` when it can't run, `409 game_busy` while it's being moved or its files deleted (a move or delete likewise waits for no launch and fails with `game_busy` or `game_running`). A Windows game left `broken` by a missing runner is provisioned again first.

- `command_wrappers` are prepended in order, first outermost. Each entry is split on spaces, so `"gamescope -W 1920 -H 1080"` is one entry. A wrapper missing from `PATH` fails with `400 wrapper_not_found`.
- `launch.env` applies under the runner's environment; the game's own `env` wins over both.
- `launch.pre_script` runs through `sh -c` before the game and blocks the request. A non-zero exit fails with `409 pre_launch_failed` and the script's output.
- `launch.post_script` runs after the game exits, however it exits. Its result is only logged.
- `launch.gamemode` registers the game with GameMode for its lifetime.

The game runs under `mira-run`, which owns the scripts, the game's output log and the session record, so a session survives `mirad` restarting. If `mira-run` is missing, the game is started directly without a session record or log.

The reply's `tracked` says whether `game.state` events will follow. It is false for a Steam game under `steam.launch_mode: "steam"` (the default), which is started through `steam -silent -applaunch <appid>` (`-silent`, on unless `steam.launch_silent` is off, keeps a Steam that wasn't running from opening its window). With `steam.track_process` on, Mira still finds the game's process by its `SteamAppId`/`SteamGameId` and records playtime, but gets no exit code. `steam.launch_mode: "direct"` runs the game through Steam's Proton build and prefix with full tracking, but needs `exe_path` set by hand.

Launching a store launcher game (Battle.net, Ubisoft, EA) asks the launcher to start it and tracks the game's own processes.

### `GET /v1/games/{id}/sessions?limit=`
`{"sessions": [{"started_at", "ended_at", "duration_seconds", "exit_code", "signal", "incomplete"}, ...]}`: the game's finished play sessions, newest first, at most `limit` (default 50; `400 invalid_param` unless a whole number, 1 or more). `incomplete` is a session mirad restarted during, so how it ended is unknown. Removing the game removes its sessions.

### `GET /v1/games/{id}/log?lines=`
`{"lines": [...]}`: the last `lines` (default 200; `400 invalid_param` unless a whole number, 1 or more) lines of the game's log, which holds its output plus `mira-run`'s own notes. Only the last 4 MB of the file is read. A game with no log returns an empty list. Each launch rotates the log to `.log.1`, unless it is over `launch.log_max_mb`.

### `GET /v1/logs/{channel}?after=&lines=`
A live log, one per task so two running at once don't mix. Channels: `daemon` (mirad's own output), `game:<id>` (the game's log file, as above), `setup:<source>` (a launcher's install or a store tool's download), `install:<source>:<ref>` (a store title's install or update) and `runner:<kind>:<name>` (a runner download). Reply: `{"lines": [...], "next": N, "active": bool}`. Send `next` back as `after` to get only what came since; with no `after` the last `lines` (default 300) are returned. `active` is whether the task is still writing. A channel nothing has written to is an empty list. `400 invalid_param` for a bad `after` or `lines`. Non-game channels are kept in memory (the last 4000 lines) and start over when the task starts again.

### `POST /v1/games/{id}/stop`
Sends SIGTERM to the game's process group and every process in its prefix, then SIGKILL after `launch.stop_timeout_s`. Proton games leave the group early, so the prefix is what reaches them. If the game isn't running, returns `{"status": "not_running"}` and publishes `game.state` with `idle`. `mirad` also publishes `idle` for every game at startup.

### `POST /v1/games/{id}/run`
Body `{"exe_path": "...", "args": "..."}`. Runs any executable in the game's prefix with normal tracking, provisioning the prefix first if there isn't one. This is how an installer is run by hand. `409 program_missing` when `exe_path` (relative to the game's folder, or absolute) isn't a file; a Windows path such as `C:\...` isn't checked.

A native program without its executable bit is `409 not_executable`. A launch or run whose program still can't be started (its interpreter is missing, exec is refused) answers `409 exec_failed` rather than `200`.

### `POST /v1/games/{id}/install`
Body (optional) `{"interactive": bool, "installer": "path"}`. Runs a `needs_install` game's installer in its prefix. Inno Setup, NSIS and MSI installers run silently with `install.inno_args`/`install.nsis_args`/`install.msi_args` and the game folder as the target; anything else is shown. `installer` (absolute or relative to `install_path`) picks the file and also works for a `broken` game. One installer runs at a time. Afterwards the game executable is looked for in `install_path` or in new folders under `install.detect_dirs` in `drive_c`. A game found in a new folder moves there, to its own folder rather than a publisher's folder around it (`Program Files/Ubisoft/<game>`): `installer_dir` keeps the old one, a game still named after the installer's folder takes the new folder's name, and its metadata is fetched again while `metadata.enabled` is on. A job (kind `install`). Events: `game.install.started`/`finished`/`failed` and `game.updated`, also for an installer a scan runs on its own (`scan.auto_run_installers`). Errors: `409 not_needs_install`, `409 install_running`, `404 installer_missing`.

### `GET /v1/games/{id}/installer[?path=]`
`{"path", "size_bytes", "format": "inno"|"nsis"|"msi"|"unknown", "silent", "silent_args"}` for the game's installer, or for `path`.

### `DELETE /v1/games/{id}/installer`
Deletes the game's `installer_dir` and clears it, returning the game. Only inside a library root, and never when that folder also holds the game's `install_path` or `data_dir` (`409 installer_dir_in_use`). `404 no_installer_dir` when there's none, `409` while the game runs.

### `GET /v1/games/{id}/install/progress`
`{"state": "idle"|"queued"|"running"|"finished"|"failed", "mode": "silent"|"interactive", "started_at", "finished_at", "error", "bytes_written"}`. Silent installers report no percentage, so `bytes_written` is the progress signal. Kept in memory only.

### `POST /v1/games/{id}/finish-install`
Marks a `needs_install` or `broken` game `ready` once `exe_path` points at the installed game. `409 no_executable` if `exe_path` is empty, still an installer, or missing.

An optional body `{"install_path"?, "exe_path"?}` switches the game to a program installed in its prefix first: `install_path` must be inside the game's `data_dir` (`400` otherwise, `409` while the game runs), and the game's candidates are detected again there. The move is handled like an install's: `installer_dir`, the name and a metadata refetch.

### `POST /v1/games/{id}/relocate`
Body (optional) `{"install_path"?, "data_dir"?}`. Moves the game's files and prefix to those paths, leaving one left out of the body where it is, or with no body into Mira's layout (`relocate.install_root` or the first library root, and `prefix_root`, named per `prefix_naming`). Targets must be inside a library root or `prefix_root`. Store games keep their install folder unless one is given, since their store tool tracks it; Lutris games don't: once moved they become `manual` games (their `source_ref` is `lutris:<slug>`), which a later Lutris import leaves alone. When one folder is inside the other (a prefix holding the game), the outer one moves and the inner one follows. A game run from an AppImage moves as that file alone, into a folder of its own, whatever else its folder holds. Otherwise an install folder that holds another game or a library root isn't moved (`shared_folder`). A program the game only runs (an executable outside the game's folder, or an AppImage handed the game's file in `args`, like an emulator) stays where it is. Moves across filesystems copy then delete, unless `relocate.allow_copy` is off. A [job](#jobs) whose result is the moved game; publishes `game.updated`.

### `POST /v1/games/{id}/tricks`
Body `{"verb": "corefonts"}`. Runs `winetricks --unattended <verb>` in the game's prefix. Fails if the game has no provisioned Wine or Proton prefix or winetricks isn't available (see `/v1/runners/tools`). A job (kind `tricks`) that runs one at a time. Events: `tricks.started`/`finished`/`failed`.

## Library

`library_roots` is an ordinary setting. Changing it through the API also updates the watcher.

### `POST /v1/library/scan`
Scans every library root now: adds new games (each folder in a root or in one of its [sorting folders](#folders-by-tag), and each AppImage loose in one), follows games whose folder was moved by hand within the library folders, marks vanished ones `missing` (or removes them with `library.remove_missing`; a root that can't be read marks nothing), restores ones that came back, and provisions games still waiting on a runner. Each change publishes its own event. A [job](#jobs) whose result is `{"added": 1, "missing": 0, "restored": 0, "moved": 0}`. The watcher runs the same scan on its own when a root changes, but only for new arrivals.

### `POST /v1/library/relocate`
Body (optional) `{"ids": [...]}`. Relocates those games, or every game without a body, into Mira's layout (inside the game's [sorting folder](#folders-by-tag) when the destination is sorted by tag), one at a time, publishing `game.updated` and `job.progress` as each one moves. A [job](#jobs) whose result is `{"moved": N, "failed": N, "errors": [{"id", "error": {...}}]}`.

### `POST /v1/library/import/classify`
Body `{"path": "/absolute/path"}`. Says what a dropped file or folder is, without moving it: `{"kind": "game" | "app" | "unknown", "name", "reason"}`. An AppImage is read from its own desktop entry (its `Categories`); anything else is looked up on Steam's store by exact name (software or game). 400 `invalid_body` for a missing or relative path, 404 `not_found` when it doesn't exist.

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

### `GET /v1/library[?source=epic|steam|gog|itch|amazon|office][&fresh=1]`
What each account owns, whether or not it's installed:

```json
[{ "source": "epic", "ref": "e8bbb84be35640cda646233152ff3428", "title": "Brotato",
   "installed": true, "game_id": "epic-e8bbb84be35640cda646233152ff3428",
   "play_seconds": 0, "owned": true }]
```

`ref` is the store's id and what `/v1/library/install` takes. `installed` and `game_id` say whether Mira tracks it as `<source>-<ref>`. `play_seconds` comes from the store (only Steam reports it). `owned` is false for a paid itch game listed from a collection the account hasn't bought. `protondb_tier`, `steam_tags` (most voted first) and `steam_reviews` (`{score_description, percent_positive, total_reviews}`, Steam's user reviews of the title or, with `metadata.steam_by_name`, of the Steam game of its name) are there when cached.

Each source's owned titles are stored in cache.db as it last listed them, so the listing answers at once; titles become games once installed. A source never listed yet is asked during the request. Every other one is re-checked in the background, one check per source at a time, between `library.catalog_checking` and `library.catalog_checked` (`changed` says whether the list differs, the cue to list again). `fresh=1` asks each source during the request instead. Signing out of a store drops its stored list. A source that isn't set up lists nothing, and `GET /v1/<source>/status` tells why. Steam needs `steam.web_api_key` and `steam.steamid64` to list games that aren't installed. Humble Bundle isn't included.

### `POST /v1/library/install`
Body `{"source": "...", "ref": "..."}`. Installs an owned title as a job (kind `install`, target `<source>-<ref>`).

- `epic`: `legendary install` into `epic.install_root`.
- `gog`: `gogdl download` into `gog.install_root/<id>`.
- `itch`: butler's install sequence.
- `amazon`: `nile install` into `amazon.install_root`.
- `steam`: opens `steam://install/<appid>`. The game appears on the next Steam scan.
- `office`: the Office Deployment Tool adds the app to Microsoft 365's prefix.

A missing tool or login fails the install with `library.install.failed`, whose `code`, `hint` and `fix` say what's needed. Afterwards the title is imported and provisioned. Events: `library.install.started`/`finished`/`failed` and `library.install.progress` (`progress` 0..1, `eta` seconds, `bps`; -1 when not reported) for Epic, GOG, Amazon and itch.

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

Tags are a game's `tags`; `favorite`, `hidden` and `app` have their own actions, so these endpoints refuse them and leave them out of their lists. Tags match ignoring case. With `tags.steam` on, a game's or store title's [metadata](#metadata) fetch also stores its Steam tags (the 20 Steam shows, most voted first) in its metadata record as `steam_tags`, from the same Steam store request as its details: a Steam game's by its appid, any other game's (with `metadata.steam_by_name`) by a Steam game of exactly its name. A store's launcher (`source` `launcher`) gets none. They're never added to a game; clients offer them.

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
Body `{"reference": "kind:name"}`. Installs the newer release like a download, then moves every game using the old build, and `default_runner.windows` if it names it, onto the new one. The old build stays. The moved games arrive as `games.updated`, then `runners.updated {kind, from, to, games}` fires before `runners.download.finished`, which then carries `replaced`. `409 no_update` when nothing is newer.

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

### `POST /v1/steam/shortcut`
Body `{"exe", "launch_options"}`. Keeps a non-Steam shortcut named "Mira" running `exe` with `launch_options` in every Steam account's `userdata/<id>/config/shortcuts.vdf`, so Big Picture can switch to Mira. An existing "Mira" entry is updated in place, other shortcuts are kept, and an unchanged file isn't rewritten. Answers `{"status": "ok", "added": [ids], "updated": [ids]}`, or `{"status": "disabled"}` with `steam.mira_shortcut` off. `404 steam_not_found` without Steam, `500 shortcuts_unreadable` for a file Mira can't parse (it's left alone). Steam shows a new shortcut after it restarts. The GUI calls this at start with its own path and `--big-screen`.

### `POST /v1/steam/bigpicture`
Opens Steam's Big Picture through `steam steam://open/bigpicture`, starting Steam if needed. Answers `{"status": "opened"}`.

### `POST /v1/steam/status`
Body `{"status": "online" | "invisible"}`. Sets the Steam friends status through `steam steam://friends/status/<status>` and answers `{"status": "invisible"}`. Steam can't report the status back, so there is no GET. Answers 409 `steam_not_running` while Steam isn't running (by `~/.steam/steam.pid`), since the URL would start the client just to set a status, and 400 `invalid_status` for any other value.

## Lutris

### `POST /v1/lutris/import`
Reads Lutris's `pga.db` (through the `sqlite3` CLI) and each game's YAML config (`lutris.data_dir` overrides where to look) and imports `wine` and `linux` runner games. A [job](#jobs) whose result is:

```json
{ "added": 3, "updated": 1, "other_runner": 2, "incomplete": 0 }
```

- `linux` games import as native with no prefix.
- `other_runner` counts games using other runners, which are skipped. Steam and Flatpak games are covered by the Steam scan and desktop entry import.
- `incomplete` counts Wine games with no `prefix` in their config and Linux games with a relative `exe`.

Nothing on disk is moved. `install_path` is the executable's folder and `data_dir` is the configured prefix. `runner_ref` is left empty so `default_runner.windows` applies, since Lutris's Wine version is often an alias. Lutris categories become tags (`.hidden` becomes `hidden`, `favorites` becomes `favorite`) and are merged with existing tags. Re-importing updates Lutris's fields and leaves overrides alone. Lutris sets `install_path`, `exe_path` and `data_dir` on the first import; after that Mira's stay (a different executable picked in Mira survives) unless what they point at is gone. `candidates` lists the executables in the game's folder, unless the folder holds other games too. Lutris's `playtime` and `lastplayed` fill `play_seconds` and `last_played_at` until Mira has recorded a session of its own (`last_session_at`); after that Mira's record stays.

## Stores

Epic, GOG, itch, Amazon and Humble each wrap a command-line tool, and all five share one set of calls under `/v1/stores/{id}`, where `id` is `epic`, `gog`, `itch`, `amazon` or `humble` (`404 store_not_found` otherwise). Store games always launch through Mira's own runners, never through the store tool.

- `GET /v1/stores`: `[{"id", "name", "tool_name", "can_import", "can_logout"}]`.
- `GET /v1/stores/{id}/status`: `{id, name, tool, authenticated, account}`. `account` is only known for Epic. `tool` is `{"installed", "source", "path", "version"}`, where `source` is `override` (the `<store>.*_bin` setting), `managed` (Mira's copy in `~/.config/mira/tools`), `path` or `none`, in that order. A missing tool is not an error.
- `POST /v1/stores/{id}/setup`: a [job](#jobs) (kind `setup`) that downloads the tool's latest release. Run it again to update. Result `{tag}`.
- `POST /v1/stores/{id}/login/begin`: `{url}`, the page to sign in at. Amazon makes a fresh one each time.
- `POST /v1/stores/{id}/login`: body `{"credential": "..."}`. Returns the status. What the credential is depends on the store:
  - Epic: the `authorizationCode`, or the whole JSON the login page shows. Legendary exits 0 on a bad code, so the result is checked through status; a rejected code fails with `400 login_failed`.
  - GOG: the `code` from the redirect URL, or the whole URL. An expired token is refreshed once.
  - itch: an API key from [itch.io/user/settings/api-keys](https://itch.io/user/settings/api-keys), checked with butler straight away.
  - Amazon: the amazon.com URL the login ends on, or its `openid.oa2.authorization_code`, after `login/begin`.
  - Humble: the `_simpleauth_sess` cookie from a logged-in browser.
- `POST /v1/stores/{id}/logout`: forgets the sign-in. `400 logout_unsupported` for Humble, whose tool keeps its own session.
- `POST /v1/stores/{id}/import`: a [job](#jobs) (kind `import`) that adds the games the store's tool reports as installed, with the store as their `source`, and provisions a prefix for each. Result `{added, updated}`. `400 import_unsupported` for Humble.

### Epic

Wraps [Legendary](https://github.com/derrod/legendary). Deleting an Epic game's files runs `legendary uninstall`.

### GOG

Wraps [gogdl](https://github.com/Heroic-Games-Launcher/heroic-gogdl), which needs `python3`. gogdl can't list owned or installed games, so the library listing uses GOG's own API with gogdl's token, and import only looks under `gog.install_root` (default `~/.local/share/mira/gog`). The listing leaves out packs, DLC and other entries that aren't installable games; a game's owned DLC installs with it (`--with-dlcs`) while `gog.install_dlc` is on, and an update adds DLC bought since.

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

Battle.net, Ubisoft Connect and the EA app have no Linux client, so each is installed into its own prefix (game `launcher-<id>`). Games installed through a launcher are imported as `<id>-<ref>` with source `battlenet`, `ubisoft` or `ea`, sharing its prefix and runner. Microsoft 365 (`office`) works the same way: its install sets up the prefix with the [mira-winapp-shims](https://github.com/Mira-Launcher/mira-winapp-shims) DLLs and Microsoft's Edge WebView2 runtime. Its apps (Word, Excel, PowerPoint, Outlook, OneNote, Access, Publisher) are then a library source like a store: `GET /v1/library?source=office` lists them and `POST /v1/library/install` installs one through the Office Deployment Tool, for the edition in `launchers.office.plan`, as `office-<app>`, tagged `app`. Deleting an app's files runs the Office Deployment Tool without it. Office signs in and checks the subscription itself; Mira never sees the account. `launchers.auto_import` imports on every scan. umu's `STORE` and, when known, `GAMEID` are set so protonfixes apply.

### `GET /v1/launchers`
`[{id, name, game_id, installed, install_state, interactive_install, prefix, runner_ref, error}]`. `install_state` is `idle`, `running`, `finished` or `failed`.

### `POST /v1/launchers/{id}/install`
Creates the prefix, runs the winetricks steps, then the installer: silent for Ubisoft and EA, shown for Battle.net. Imports games afterwards. A job (kind `install`). `409 install_running`. Events: `launcher.install.*`; `launcher.install.progress` (`progress` 0..1) when the installer reports how far along it is (Microsoft 365 does).

### `POST /v1/launchers/{id}/import`
A [job](#jobs) whose result is `{added, updated}`. Battle.net games are found by their default folders, Ubisoft games by registry keys and EA games by `__Installer/installerdata.xml`. `409 launcher_not_installed`.

### `POST /v1/launchers/{id}/open`
Body `{action?: "launch"|"install", ref?}`. Opens the launcher, or asks it to launch or install a game by store id (a Battle.net product code such as `WTCG`, a Ubisoft id or an EA offer id). Not tracked.

## Sources

### `GET /v1/sources/{id}/removal`
What removing a source would do:

```json
{ "source": "ubisoft", "games": [ { "id": "ubisoft-5595", "name": "Trackmania",
    "deletes": "/home/me/Mira/prefixes/ubisoft-connect/drive_c/.../Trackmania" } ],
  "launcher_dir": ".../drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher",
  "kept": [ "/home/me/Mira/prefixes/ubisoft-connect", ".../Ubisoft Game Launcher/savegames" ],
  "signs_out": false }
```

`deletes` is empty for games that are only dropped from Mira (Steam, Lutris and Humble own their files).

### `POST /v1/sources/{id}/remove`
Uninstalls the source's games (through the store tool, or by deleting a folder inside a Mira folder), deletes a launcher's program folder but keeps save folders, signs out, removes the games from Mira and sets `<id>.enabled` to false. Prefixes are never deleted. A [job](#jobs); a failed step is reported and the rest still run: `{"removed": 3, "problems": []}`.

### `GET /v1/sources/{id}/runner`
The runner a source's games use: `{"runner_ref", "games", "differing"}`. `games` counts its Windows games and `differing` the ones on another runner. For Epic, GOG, itch and Amazon this is `<id>.runner`, the default for their games with no runner of their own (empty falls back to `default_runner.windows`). For a launcher it is the runner of its prefix, which its games share. `400 no_runner` for Steam, Lutris and Humble; `409 launcher_not_installed`.

### `POST /v1/sources/{id}/runner`
Body `{"runner_ref", "apply_to_games"?}`. `runner_ref` is `kind:name`, `auto` or empty, and must resolve to an installed build. A store's default changes, plus every Windows game from it when `apply_to_games` is true. A launcher and the games in its prefix always change together. Returns the new state as above and publishes `game.updated` for each game it moved.

## Desktop entries

Imports existing `.desktop` entries as games. Mira's own `mira-<id>.desktop` entries are controlled by the `desktop_entries.*` settings. Nothing is imported automatically.

### `GET /v1/desktop-entries/candidates`
Entries from `$XDG_DATA_HOME/applications`, `$XDG_DATA_DIRS`, the Flatpak export directories and `desktop_import.extra_dirs`:

```json
[{ "id": "com.spotify.Client", "name": "Spotify", "icon": "com.spotify.Client" }]
```

`id` is the desktop file ID. Skipped: non-applications, `NoDisplay` or `Hidden` entries, Mira's own entries, Steam game shortcuts, and entries already in the library.

### `POST /v1/desktop-entries/import`
Body `{"ids": [...]}`. A Flatpak entry becomes `flatpak run <app-id>`. Anything else uses its `Exec=` line with field codes removed. The command is stored as an absolute `exe_path`: a bare one is looked up on `PATH` when importing, and an entry whose command isn't installed isn't offered. Imported games are native, keep the entry's id in `source_ref`, and get a `desktop_entries.enabled: false` override, since the app already has a menu entry. Importing the same entry again updates it and keeps that override if it's set. Returns `{"added": 1, "updated": 0}`.

### `POST /v1/desktop-entries/sync`
Rewrites Mira's own desktop entries now.

## GameMode

### `GET /v1/gamemode/status`
`{"installed": true, "daemon_running": false}`. `installed` means `gamemoded` or `gamemoderun` is on `PATH`; `daemon_running` means GameMode owns its D-Bus name. With `launch.gamemode` on, an unreachable daemon is logged and the game still launches.

## Metadata

Store info is cached in `cache.db` in the config directory, and art as files under `artwork/<id>/`, with `cache.db` pointing at each slot's file. All of it can be fetched again, so a damaged `cache.db` is started over. Sources:

- **Steam games**: Steam's store API, review summary and CDN art (`cover`, `hero`), plus the ProtonDB tier. No key needed.
- **GOG, itch and Amazon**: cover and hero from GOG Galaxy's games database, with nile's cached art as a fallback for Amazon.
- **Everything else**: [SteamGridDB](https://www.steamgriddb.com) by name (`cover`, `hero`, `logo`, `icon`) when `steamgriddb.api_key` is set. Without a key, `metadata.steam_art_by_name` borrows art from a Steam game of the same name. If nothing is found, the fetch fails with `no_steamgriddb_key`.

With a key, SteamGridDB also adds alternates for every game in `art_candidates`, without replacing store art. `metadata.steam_by_name` (on by default) looks up a ProtonDB tier by name for non-Steam games, and Steam's reviews and tags for a Steam game of exactly the same name. A game's `metadata.steam_appid` replaces the name match.

Art is shrunk as it's saved to fit its slot (`metadata.art_size`): a cover within 900x1350, a hero within 1920 wide, a logo within 800 and an icon within 256, or about two thirds of that when compact. It's saved as JPEG, or PNG when it has transparency. A JPEG that already fits is kept as downloaded.

New games are fetched when first added, through a queue of three workers. Tracked games go before store titles. `metadata.enabled` turns automatic fetching off. Details (store info, reviews, ProtonDB tier) older than `metadata.refresh_days` are fetched again on start, without the art. `details_fetched` is when they last were.

### `GET /v1/games/{id}/metadata`
The cached JSON: `source`, `fetched_at`, `details_fetched`, and whichever of `steam`, `steam_reviews`, `epic`, `protondb`, `artwork` (the cover), `hero`, `logo` and `icon` were found. Art entries look like `{"file", "content_type", "source", "candidate_id"?, "chosen"?}`; `chosen` marks a slot the user picked. `art_candidates` maps each slot to `[{"id", "url", "thumb", "width", "height", "style", "nsfw"}]`; adult art is only listed with `steamgriddb.nsfw` on and is never picked by default. `steam` includes `controller_support` (`"full"`, `"partial"` or `""`) from Steam's store. `404` when nothing is cached.

### `GET /v1/games/{id}/artwork?type=`
The cached image for a slot (`cover` by default). `404` if that slot isn't cached.

### `POST /v1/games/{id}/artwork?type=`
Body `{"candidate_id": <id>}`. A job (kind `artwork`) that switches a slot to a cached candidate. Only candidate ids are accepted, never URLs. A metadata refresh keeps the pick; picking again is the only way to change it. Events: `game.artwork_selected`/`artwork_select_failed`.

### `POST /v1/games/{id}/artwork/candidates?type=&page=&request=`
Fetches one page (50) of SteamGridDB art for a slot, starting at page 0, and adds it to `art_candidates`. Event: `game.artwork_candidates_ready` with `{id, type, page, request, total, candidates}`, or `code` and `error` (`no_steamgriddb_key`, `no_steamgriddb_match`, `steamgriddb_unreachable`). `request` is echoed back so a caller can match its answer.

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
Body `{"steamgriddb_id": N}`. Uses that SteamGridDB game from now on; `0` goes back to the top match.

### `POST /v1/games/metadata/refresh`
Body `{"ids": [...]}`. Fetches each of these games again, like `POST /v1/games/{id}/metadata/refresh` without `announce`. Unknown ids are skipped. A job (see [Jobs](#jobs)) of kind `metadata`: `job.progress` counts games as their fetches end, and the result is `{"refreshed", "failed"}`. Each game still sends its own `game.metadata_ready`/`metadata_failed`.

### `POST /v1/games/metadata/refresh-missing`
The same, for every game without a cover.

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
| `games.removed` | `{ids}`, from `POST /v1/games/delete`. |
| `config.changed` | `{keys, frontend}` after `PATCH /v1/config`, a reset, or a hand edit of `settings.toml`: the dotted settings keys that changed (names only, never values) and the whole `frontend` table, so a client applies a change made elsewhere. |
| `game.state` | The game plus `state` (`running`, `exited`, `crashed`, `idle`) and, after an exit, `exit_code`, `signal`, `played_seconds` and `error`. `crashed` means a crash signal (or a shell's 128 + one), exit code 126/127 or a program Wine couldn't load, or Wine's "Unhandled ..." report in the log followed by a non-zero exit; a plain non-zero exit is `exited`, and so is anything after a stop. A crash adds `code` (`crashed`, `killed` or `start_failed`), a plain-language `error` that is also the game's `last_error`, a `hint` and a `fix` that opens the game's log. |
| `game.install_detected` | `{id, install_path, exe_path}`, after a launched Windows game exits and its prefix gained a program folder, i.e. the "game" was an installer. `exe_path` is relative to `install_path`, empty when no program was found. Adopt it with `finish-install`. |
| `game.installer_leftover` | `{id, installer_dir, bytes}`, after an install or `finish-install` left the game somewhere other than its installer's folder, which is still on disk. Delete it with `DELETE /v1/games/{id}/installer`. |
| `game.launched` | `{id, via, tracked}` for launches handed to Steam or a store launcher. |
| `game.install.*` | See `POST /v1/games/{id}/install`. |
| `game.metadata_ready`, `game.metadata_failed` | See metadata refresh. |
| `game.artwork_*` | See the artwork endpoints. |
| `tricks.*` | See `POST /v1/games/{id}/tricks`. |
| `library.install.*` | `{source, ref, update}`; `progress` adds `progress`, `eta` and `bps`. |
| `library.artwork_ready`, `library.artwork_failed` | `{source, ref}`, plus the error fields on failure. |
| `library.catalog_checking`, `library.catalog_checked` | `{source}` while `GET /v1/library` re-checks a store's owned titles; `checked` adds `changed`. |
| `job.started`, `job.progress`, `job.finished`, `job.failed` | See [Jobs](#jobs). |
| `runners.download.*`, `runners.updated`, `runners.removed` | See the runner endpoints. |
| `umu.setup.*`, `winetricks.setup.*` | Tool installs. |
| `launcher.install.*` | See `POST /v1/launchers/{id}/install`. |
| `notification` | A message for the user, with its level. |

`mira watch` in `src/cli/main.cpp` is a minimal client.
