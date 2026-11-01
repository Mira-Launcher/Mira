# The backend (`mirad`)

Mira is a Linux game launcher for native games and Windows games run through Wine or Proton. It is three programs:

| Program    | Role |
|------------|------|
| `mirad`    | The daemon. Owns all state and does all the work. |
| `mira`     | Command-line client. |
| `mira-gui` | Qt frontend. |

The only link between them is the REST API in [`api.md`](api.md), served over a Unix socket. `mira-gui` does not link against `mira_core` and never reads `settings.toml` or the library database itself. If the frontend needs something the API doesn't offer, add an endpoint.

Every endpoint gets a matching `mira` command, so anything the GUI can do can also be done from a terminal.

## Code layout

```
src/
  core/       Result<T>, Command (argv + env + cwd), Paths (XDG), Strings, Log, BackgroundQueue, TOML/JSON helpers
  config/     Schema (every setting declared once), Config (settings.toml and frontend.toml),
              Resolver (default -> file -> per-game lookup), KnownExePatterns and RunnerSources (plain data)
  model/      Game, RunnerBuild, Event, Candidate: plain structs with ToJson/FromJson
  store/      GameStore and MetadataStore, over SQLite (mira.db, cache.db)
  library/    Detector, Scanner, AutoSetup, AutoInstall, Watcher, ArchiveExtractor, WinePrefix,
              PrefixNaming, Relocate, FolderTags, ILibrarySource, SourceRegistry, Catalog, SourceRemoval
  runner/     IRunner with Native, Proton, Wine and Steam runners, RunnerRegistry, Downloader,
              Exec, GameMode, Winetricks
  proc/       ProcessSupervisor, Session, ExitReason, Stats, ProcessIndex
  desktop/    DesktopEntries (menu entries for games) and DesktopEntryScanner (import existing entries)
  steam/      Vdf parser, SteamDetector, SteamScanner, SteamSource, SteamWebApi, FriendsStatus,
              Achievements (Steam's stats files), GridArt (picks into Steam's library)
  epic/       Legendary wrapper, importer, installer, source and achievements (library/Stores.cpp lists the five stores)
  gog/        gogdl wrapper, importer, installer, source, achievements (GOG Galaxy's gameplay API) and comet
  itch/       butlerd JSON-RPC client, importer, installer and source
  amazon/     Nile wrapper, importer and source
  humble/     humble-cli wrapper (downloads only, not an ILibrarySource)
  launchers/  Battle.net, Ubisoft Connect, EA app, Microsoft 365 and Creative Cloud, each installed into its own prefix
  emulators/  The emulator table (install route, ROM extensions, firmware, launch args) and the ROM folder importer
  modmanagers/  Vortex and Mod Organizer 2: download, adding to a game, and Purge (undoing a manager's deployment and deleting its data for one game)
  lutris/     Lutris library import
  heroic/     Heroic Games Launcher import
  metadata/   Cover art and store metadata fetching
  achievements/  Achievement lists from each store, the system locale, and achievement icons
  saves/      The save locations list (download, index, matching a game to an entry), resolving a game's save files, and their backups
  setup/      `mira setup`: wrapper script, desktop entry and icon for the AppImage
  system/     Packages: the distro packages each feature needs, and the command that installs them
  updates/    What has a newer version: Mira, runners, store tools and emulators
  migrate/    One-time import of Mira 0.13's files (temporary)
  api/        EventBus (in-memory pub/sub), Jobs, Services and Server; routes/ holds the REST routes by area
  cli/        `mira`
  wrapper/    `mira-run`, the process that owns a launched game's session
  mirad_main.cpp

frontend/     mira-gui, see frontend.md
tests/        doctest unit tests (mira_tests)
packaging/    desktop entry, icons, PKGBUILD
cmake/        AppImage build
```

Route files register through a `Router`, which runs every handler behind one gate that sees the parsed body (httplib's pre-routing handler runs before the body is read). While hidden games are locked, the gate answers a request naming one as a missing game; list routes skip them through `Services::Conceals`, and `EventBus`'s filter drops or rewrites events about them.

## Storage

All user state lives in `$XDG_CONFIG_HOME/mira` (normally `~/.config/mira`):

```
settings.toml   backend settings, validated against config/Schema.cpp
mira.db         the library (SQLite; mira.db.bak is a copy from the last start)
cache.db        fetched store info and which artwork/ file is each art slot's (SQLite)
artwork/        downloaded art, artwork/<id>/<slot>.*
frontend.toml   GUI settings, stored and returned verbatim by the backend
logs/           per-game output from the last launches
```

`settings.toml` is TOML so people can edit it, also while mirad runs: a saved edit applies at once (`Services::SettingsChanged`, which a request's change goes through too), and an app change re-reads the file first so it only changes its own keys. An edit that doesn't parse changes nothing and sends a notification naming the line. At startup such a file is renamed to `.bad`, and mirad goes back to the last settings that loaded (kept in `mira.db`), or to defaults. The file holds only the values that were set (`Config::set_`; the defaults with those over them are what's read), under a header line saying so; a value is removed by a reset or a `null` in a patch, and tables left empty go with it. Older versions wrote every default out, so a file without the header is cleaned once on load: values equal to today's default or to one the schema entry lists as earlier (`Entry::old_defaults`) are dropped, and the file as it was is kept as `settings.toml.with-defaults`. The library and prefix folders list no earlier defaults, since games live in the folders an install started with. A changed default gets its old value added to `old_defaults`, for files still to be cleaned. Every load also drops what no setting reads (settings since removed, the old `[frontend]` table), naming them in the log and keeping the file as it was as `settings.toml.with-removed-settings`; a migration of a setting that moved elsewhere reads `Config::FileAsLoaded`, the values before that. `GET /v1/config` sends a secret (`Entry::is_secret`) as `""` and lists the set ones in `secrets_set`, unless asked with `?reveal=1`.

**mira.db** (`store::GameStore`): `games` (JSON text for `runner_config`, `overrides`, `env`, `candidates`), `game_tags` (removed with their game), `sessions` (kept when their game is removed, so a game added again under its id gets its play history back; `mira-run` writes a session's row through its own connection, and mirad marks it counted), `removed_games` (what the user set on each removed game, as JSON, which `Reattach` puts back on a game added under its id; `NextId` gives a game found at a removed one's `install_path` that id), `settings_snapshot`, `ui_state` (the GUI's window state, kept out of `frontend.toml`), and `hidden_titles` (store titles hidden while not installed; a hidden title's game takes the `hidden` tag as it's added, and a removed hidden store game puts its title back), `secrets` (values clients can't read through settings.toml: the hidden games' password hash), and `cloud_sync` (each cloud save location's state at its last check and sync; kept on a plain removal, dropped with `delete_metadata`). Each change is a transaction, then updates an in-memory copy that answers reads. Each start runs `PRAGMA quick_check` and writes `mira.db.bak`; a damaged file is renamed to `.bad` and the backup used.

**cache.db** (`store::MetadataStore`): fetched info per id, games and store titles alike, plus `artwork` rows pointing at each slot's file under `artwork/<id>/`, `lists` of what's fetched for the whole library (Steam's tag names), `achievements` per game (icons under `artwork/<id>/achievements/`, dropped with the game's metadata), and `cloud_saves`, where each GOG or Epic game keeps its cloud saves, dropped likewise. `save_entries` and `save_ids` index the save locations list: a rebuild writes the next generation of rows in batches beside the current one, switches the `lists` row `save_entries` to it and only then deletes the old rows, so reads never wait out a rebuild or see half of one. Everything in it can be fetched again, so a damaged one is started over.

Only mirad opens either database. Schema changes are steps appended to `kMigrations`, counted by `PRAGMA user_version`; never edit a shipped step. Files from Mira 0.13 (`games.toml`, `metadata/*.json`, `sessions/*.toml`, window state in `frontend.toml`) are imported once by `src/migrate/`, which is temporary: delete that folder, its call in `mirad_main.cpp` and its test to drop it.

## Running the daemon

`mirad` never daemonizes itself, and users don't run it:

1. **Started by the GUI.** `frontend/app/DaemonSupervisor` probes `GET /v1/health`. If nothing answers, it starts `mirad` from next to its own binary or from `PATH` and stops it again on quit. A daemon it didn't start is left alone. Closing the window hides Mira to the tray, so `mirad` keeps running; quitting while installs run asks first. `mira-gui` holds a `QLockFile` at `$XDG_RUNTIME_DIR/mira/mira-gui.lock` so a second launch exits instead of opening another window.
2. **Started for `mira`.** A `mira` command that finds nothing listening runs `mira-gui --hidden` (in the tray, or minimized with no tray) on a desktop, or `mirad` for the length of the command without one. A game menu entry runs `mira launch`, so it works with Mira closed.

Playtime doesn't depend on either staying up: `mira-run` writes each session's record itself.

`packaging/mira.desktop` launches `mira-gui`. There is no menu entry for `mirad`.

## Idle cost

The daemon should cost nothing when idle:

- `EventBus::WaitNext` waits on a condition variable. The 20-second timeout only runs while an SSE client is connected, to notice when it goes away.
- Shutdown uses `sigwait`, not a polled flag. Stopping wakes every background wait at once (open event streams, the external-game watcher, the per-game exit watchers), so quitting never waits out a poll interval.
- Background work runs on `Lane`s (`src/core/Lane.h`): named pools whose threads start when work arrives and exit after 30 idle seconds, so an idle mirad keeps only a handful of threads.
- `butlerd` (itch.io) stops after five quiet minutes and starts again on the next call.
- The external-game watcher scans `/proc` every 3 seconds while there are Steam or store-launcher games to look for, and every 15 otherwise. It rebuilds that list only when `GameStore::Revision()` or `Config::Revision()` changes.
- `library::Watcher` blocks in `epoll_wait` on inotify, an eventfd and a timerfd. The timer is armed only while a new folder is still growing, to wait until a copy finishes. There is one non-recursive watch per library root and per sorting folder in it.

## Detection and scanning

`library::Detector` scores the executables in one game folder using the `detect.*` settings. Each rule in `detect.rules` is a plain function run in the listed order, and removing a rule from the list disables it. Windows candidates are `.exe` and `.msi` files; Wine runs an `.msi` through `msiexec` and a `.bat` or `.cmd` through `cmd`. A candidate is flagged `is_installer` when the first or last 2 MB carry an installer builder's mark (Inno Setup, NSIS, WiX Burn, InstallShield, MojoSetup; `library::IsBuiltInstaller`, which shares its sniff with the silent-install check) and its name isn't in `detect.deny_name_patterns` (which covers their redistributables); only files named like installers or at most one folder deep are opened for this. Uninstallers (`unins*`, `uninst*`) never count. A marked file with a file of at least `detect.installer_min_size_mb` beside it is a package, whatever the deny list says: the folder isn't installed yet, so it's ranked above everything else (the checksum tool a repack ships, say) and picked with full confidence. A plain executable at the package's level or above it means the game is installed after all, and the package is then scored as an ordinary installer. Otherwise an installer name (`detect.installer_name_patterns`) plus size (`detect.installer_min_size_mb`) flags it. An `.msi` always is. A game whose best candidate is an installer is stored `needs_install`. The default deny and installer patterns live in `src/config/KnownExePatterns.h`.

`library::Scanner` treats each folder directly under a library root as one game. A folder already known by `install_path` is never detected again, so a scan never overwrites a user's changes. `prefix_root` and anything that looks like a Wine prefix are skipped. Folders that disappear are marked `missing`, or removed when `library.remove_missing` is on. A root that can't be listed (permissions, a stale network mount) skips that step, so it never looks empty.

In a root listed in `tags.sorted_roots`, games are sorted into `.hidden` and a folder per tag in `tags.folders` (`library/FolderTags`): a game goes in its first tag in that list's order, or the one picked for it (`Game::folder_tag`). The scan lists those folders' games as well, and tags a new game by where it was found. Before adding a new folder it tries to match it to a known game whose folder is gone (its executable is inside), and follows that game instead, rewriting its tags to the new place; when several games fit, `library::UnclearMoves` (owned by `Services`, filled by every scanner) holds the folder until a client settles it. A game installed inside its prefix is sorted by a link (`Game::library_link`), which scans and the watcher never follow; any other link to a folder is scanned as before. `library::SortRules` is the settings sorting reads, so the preview endpoint can try folder tags that aren't saved. Going the other way, `Services::SortByTags` renames a game's folder to where its tags put it after a tag change, a settings change, the game's exit or mirad's start. Both ways prune the sorting folders left without games. Folders and tags stay one state: there's no sync step, since a rename within a drive is instant. A game's Steam tags are fetched with its metadata (`metadata::FetchSteamTags` for the library at once) and kept in its metadata record, never on the game; `GET /v1/tags` (`api/routes/TagsRoutes`) offers them beside the library's own tags.

`library::AutoSetup` stores a new game and publishes `game.added` before any provisioning. Native games are stored `ready`, Windows games `setting_up`.

`library::Watcher` runs `Scanner::ScanRoot` when a root changes. A new folder is scanned once its size stops changing for `scan.debounce_ms`. A deletion is scanned right away.

## Runners

`runner::IRunner` has `kind()`, `Discover(config)`, `Provision(game, build)` and `BuildCommand(game, build)`. There are five runners:

- `NativeRunner` runs the executable directly.
- `ProtonRunner` runs Proton builds through `umu-run`. It uses the `umu-run` on `PATH`, or Mira's own copy under the tools folder.
- `WineRunner` runs the system Wine or any Wine build it finds.
- `SteamRunner` runs a Steam game with the Proton build and prefix Steam already set up for it.
- `EmulatorRunner` runs a ROM in its source's emulator, with that emulator's launch args.

Neither `umu-run` nor `wineboot` gives a reliable exit code for prefix creation, so both runners check that `drive_c` exists afterwards.

`RunnerRegistry` turns a `kind:name` reference into a runner and build. `latest` picks the newest build. `auto` (the default for `default_runner.windows`) prefers a distro-packaged build, then GE-Proton or Wine staging-tkg, then the newest, and uses Proton when any Proton build is installed. Provisioning pins the resolved build onto the game's `runner_ref`, so installing another build never changes a working game. A pinned build that's gone (removed after a newer one of its line came) resolves to the newest installed build whose name differs from it only in its numbers, so `GE-Proton11-7` runs on `GE-Proton11-9` (`RunnerRegistry::StandIn`). Updating a build through Mira (`POST /v1/runners/update`) moves every reference to it onto the new one: games' `runner_ref`, their overrides and every runner setting (`runner::RepointRefs`, which the startup migration of in-place builds uses too). A build removed any other way (`DELETE /v1/runners/...`, by hand, or before that existed) is moved off the same way by `runner::RepointRemovedBuilds`: at startup, after a removal, and when a launch has to use a stand-in, so a game shows the build it runs. Runner settings left at their default aren't moved: they keep following the default.

Windows games are provisioned when they are detected and again on later scans while they are still `setting_up`. A game that broke because no runner was available is provisioned again on each scan, when a runner download finishes, and when it is launched.

`runner::Downloader` lists and installs Proton and Wine builds from GitHub releases (sources in `src/config/RunnerSources.h`) and checks their checksums when the release has them.

## Launching

A direct launch goes through `mira-run` (`src/wrapper/main.cpp`). `mirad` resolves the runner, applies `command_wrappers` and `launch.env`, and passes the command to `mira-run`. It waits only for a short status handshake over a pipe.

`mira-run` owns the session: `launch.pre_script`, the game process, `launch.post_script`, the game's output in `logs/<id>.log`, and a session record in `mira.db` written before and after. The session completes even if `mirad` dies in the meantime. `launch.gamemode` registers the game with GameMode over D-Bus through `gdbus`.

A store source can add to a launch through `ILibrarySource::BeforeLaunch`, which returns `LaunchExtras`: arguments, environment, argument prefixes `mira-run` masks in its log (`--redact`), and files `mirad` deletes when the game exits. Epic uses it for its sign-in arguments. A failure there is logged and the game starts without them. `LaunchExtras::shared` is a program the store's running games share, named by `ILibrarySource::SharedProgram`: GOG's comet (`src/gog/Comet.cpp`). `ProcessSupervisor::StartShared` runs it in a systemd scope named after the program and a hash of the data folder, so a test's or another Mira's never clashes, before the game starts; the exit hook stops it once no game of that store is running, and waits for it to end. A scope left from before mirad restarted is stopped by the next exit of one of its games; comet's `--quit` also ends it once the games that connected have gone.

`mira-run` shares its process group with the game, so `kill(-pid)` stops the whole tree. It ignores SIGTERM and SIGINT so it can still run the post script and write the final record.

`proc::ProcessSupervisor` waits on `mira-run` and reads the finished session record, plus the end of the game's log for Wine's crash report, and `proc::ClassifyExit` decides between a crash and an ordinary exit for every launch path. At startup, `Reconcile()` archives finished sessions, re-adopts any `mira-run` still alive and closes the rest as `incomplete`. A finished session is added to the game's `play_seconds` once: the game remembers the newest session it counted (`last_session_at`).

If `mira-run` can't be found or started, `mirad` launches the game directly with no session record or log.

## Saves

Where a game keeps its saves comes from the save locations list (`src/saves/Manifest.cpp`, indexed in `cache.db`); everything done with them is Mira's own. `saves::FindEntry` picks a game's entry, and `saves::PlanFor` decides whether Mira keeps its saves at all:

| Game | Mira backs up its saves |
|---|---|
| Steam, started by the Steam client, with Steam Cloud in its entry | No, while `saves.skip_cloud` is on: Steam Cloud keeps them |
| Steam started directly, or without Steam Cloud | Yes |
| Anything else with save locations | Yes |
| Only registry saves | No |

Registry saves aren't kept: a game's registry lives in its prefix's `user.reg` with every other program's settings, so putting back an old copy would undo those too.

`saves::Find` (`src/saves/Resolve.cpp`) resolves one location. Placeholders in the list's, GOG's and Epic's forms map to the same folders (`saves::RootsFor`): the prefix's Windows user folder (`library::WindowsUserDir`: `steamuser` once Proton made it, else `$USER`), the install folder, a Steam game's library and Steam's own folder, or the home and XDG folders for a native game. The glob walker matches one path component at a time, ignoring case for a Windows game, follows symlinks only while matching the location's own folders, and never enters `dosdevices`. A location that would take a whole prefix, home or user folder, or one of their shared folders, is refused, also when a symlink leads there (Wine links a prefix's Documents to the home folder's). Each found file is described by a logical root (`install`, `prefix`, `prefix_user`, `steam_library`, `steam`, `home`; the deepest holding it) plus a path inside it, never an absolute path, so a copy taken now still lands right after the game or its prefix moves or its runner changes between Wine and Proton.

Backups (`src/saves/Backups.cpp`) live outside every game's folders, in `saves.backup_root` or the first Mira folder's `saves`, so they don't move or go with a game's files; `saves` is one of the Mira-folder names a drop or scan never takes for a game. `saves::TakeSnapshot` writes a snapshot into `<id>.partial` and renames it when complete, so a crash or cancel leaves no half snapshot (the next backup clears the leftover). Files unchanged since the newest snapshot (path, size and mtime) are hard links to its copies, and copies keep their mtimes, which is also how "nothing changed" is decided without reading a byte. Every backup holds the game's launch claim (`Services::BackUpSaves`), so it can't overlap a session, a move, a delete or another backup of the same game. After a session, the exit hook posts the backup to the `saves` lane after `game.state` went out, so exiting isn't slowed; a failure there is kept in the game's `state.json` for the GUI rather than lost in a job nobody watched. A restore (`Services::RestoreSaves`) snapshots what's there first (`before_restore`), checks every file has a folder to go to before writing any, and puts each one in place by copying beside it and renaming over it, so a file is never left half written.

Cloud saves (`src/saves/Cloud.cpp`) go through a `saves::CloudSide` per store (`gog/CloudSaves.cpp`, `epic/CloudSaves.cpp`): where the game's saves go, what the cloud holds, and moving one location one way. `saves::RunCloud` classifies each location itself, from the state it stored in `mira.db`'s `cloud_sync` at the last sync (not re-fetchable, unlike `cache.db`), and only then calls the tool with a forced direction: gogdl calls a conflict a sync, and legendary has no conflicts at all. For GOG, the game's own client id and secret come from its newest build's manifest (zlib JSON, inflated with the `python3` gogdl runs on, since mirad doesn't link zlib) and are cached with the locations in `cache.db`; the listing needs a token for that client, which `gogdl auth --client-id` gets from the account's sign-in, so Mira never handles the refresh token. Nothing a tool prints goes into an error, since gogdl's tracebacks can hold the token URL.

The launch route syncs before starting the game (`Services::SavesBeforeLaunch`), under the launch claim it already holds. The timeout is a timer thread stopping the sync through `RunCancellable`, so `RunAndWait` kills whichever tool is running; once a download begins (the `before_download` callback, under the same mutex as the timer) the timer stands down, since stopping a download would leave saves half replaced. Syncing is always classified afresh, so a launch that went ahead unsynced needs no marker: the sync after play finds whatever changed. After play, the exit hook posts the backup and then the sync to the `saves` lane, and sorts the game by its tags only once both are done, so their claim can't make that move skip the game. All of this goes through `CloudFor(game)` in `Services` rather than a per-store hook, since the classification and the claims are the same for every store.

## Desktop entries

`desktop::DesktopEntries` writes a `mira-<id>.desktop` entry for each ready game and keeps them in sync after scans and game changes. `desktop_entries.*` settings are resolved per game, so a game can opt out. `Exec=` always goes through `mira launch <id>` (or the GUI), never the game's executable, so playtime and crash tracking always apply. It only touches files it created.

`steam::SyncGridArt` does the same for Steam's library with `steam.sync_art` on: a Steam game's picked cover, hero, logo and icon are copied into each account's `config/grid/` under Steam's names (`<appid>p`, `_hero`, `_logo`, `_icon`), after each pick and for the whole library when the setting turns on. Only picks go, since Mira's automatic art for a Steam game is Steam's own; a format Steam can't show (WebP) is left out. `steam-art.json` beside `settings.toml` lists the files Mira wrote, so it never replaces or removes any other: art set in Steam itself is renamed to `<file>.before-mira` and put back when the pick is removed or the setting turns off.

