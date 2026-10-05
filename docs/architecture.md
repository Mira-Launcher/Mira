# Architecture

Mira is a Linux game launcher for native games and Windows games run through Wine or Proton. It is three programs:

| Program    | Role |
|------------|------|
| `mirad`    | The daemon. Owns all state and does all the work. |
| `mira`     | Command-line client. |
| `mira-gui` | Qt frontend. |

The only link between them is the REST API in [`api.md`](api.md), served over a Unix socket. `mira-gui` does not link against `mira_core` and never reads `settings.toml` or `games.toml` itself. If the frontend needs something the API doesn't offer, add an endpoint.

Every endpoint gets a matching `mira` command, so anything the GUI can do can also be done from a terminal.

## Code layout

```
src/
  core/       Result<T>, Command (argv + env + cwd), Paths (XDG), Strings, Log, BackgroundQueue, TOML/JSON helpers
  config/     Schema (every setting declared once), Config (settings.toml and frontend.toml),
              Resolver (default -> file -> per-game lookup), KnownExePatterns and RunnerSources (plain data)
  model/      Game, RunnerBuild, Event, Candidate: plain structs with ToJson/FromJson
  store/      GameStore, backed by games.toml
  library/    Detector, Scanner, AutoSetup, AutoInstall, Watcher, ArchiveExtractor, WinePrefix,
              PrefixNaming, Relocate, ILibrarySource, SourceRegistry, Catalog, SourceRemoval
  runner/     IRunner with Native, Proton, Wine and Steam runners, RunnerRegistry, Downloader,
              Exec, GameMode, Winetricks
  proc/       ProcessSupervisor, Session, Stats, ProcessIndex
  desktop/    DesktopEntries (menu entries for games) and DesktopEntryScanner (import existing entries)
  steam/      Vdf parser, SteamDetector, SteamScanner, SteamSource, SteamWebApi
  epic/       Legendary wrapper, importer, installer and source
  gog/        gogdl wrapper, importer, installer and source
  itch/       butlerd JSON-RPC client, importer, installer and source
  amazon/     Nile wrapper, importer and source
  humble/     humble-cli wrapper (downloads only, not an ILibrarySource)
  launchers/  Battle.net, Ubisoft Connect and EA app, each installed into its own prefix
  lutris/     Lutris library import
  metadata/   Cover art and store metadata fetching
  setup/      `mira setup`: wrapper script, desktop entry, icon and systemd unit for the AppImage
  api/        EventBus (in-memory pub/sub) and Server (the REST routes)
  cli/        `mira`
  wrapper/    `mira-run`, the process that owns a launched game's session
  mirad_main.cpp

frontend/     mira-gui, see frontend.md
tests/        doctest unit tests (mira_tests)
packaging/    systemd unit, desktop entry, icons, PKGBUILD
cmake/        AppImage build
```

Routes live in `Server::RegisterRoutes` in `src/api/Server.cpp`. If `api.md` disagrees with it, the doc is wrong.

## Storage

All user state lives in `$XDG_CONFIG_HOME/mira` (normally `~/.config/mira`):

```
settings.toml   backend settings, validated against config/Schema.cpp
games.toml      the library, one [[game]] per entry
frontend.toml   GUI settings, stored and returned verbatim by the backend
sessions/       records for sessions still running, or finished but not yet counted
logs/           per-game output from the last launches
```

TOML was picked over SQLite so the files stay readable, editable by hand and easy to back up. `Config` and `GameStore` keep their data in memory behind a mutex and rewrite the file on every change. A file that fails to parse is renamed to `.bad` and the daemon starts with defaults.

The socket is at `$XDG_RUNTIME_DIR/mira/mirad.sock`. `mirad --socket` and `$MIRA_SOCKET` override it.

## Running the daemon

`mirad` never daemonizes itself. There are three ways to run it:

1. **systemd user service.** `packaging/mirad.service` is a `Type=simple` unit with `Restart=on-failure`. The library stays watched with no window open. Logs go to `journalctl --user -u mirad`.
2. **Started by the GUI.** `frontend/ui/DaemonSupervisor` probes `GET /v1/health`. If nothing answers, it starts `mirad` from next to its own binary or from `PATH` and stops it again on quit. A daemon it didn't start is left alone. `mira-gui` holds a `QLockFile` at `$XDG_RUNTIME_DIR/mira/mira-gui.lock` so a second launch exits instead of opening another window.
3. **By hand.** Run `mirad` in a terminal and stop it with Ctrl-C.

`packaging/mira.desktop` launches `mira-gui`. There is no menu entry for `mirad`.

## Idle cost

The daemon should cost nothing when idle:

- `EventBus::WaitNext` waits on a condition variable. The 20-second timeout only runs while an SSE client is connected, to notice when it goes away.
- Shutdown uses `sigwait`, not a polled flag. Stopping wakes every background wait at once (open event streams, the external-game watcher, the per-game exit watchers), so quitting never waits out a poll interval.
- `library::Watcher` blocks in `epoll_wait` on inotify, an eventfd and a timerfd. The timer is armed only while a new folder is still growing, to wait until a copy finishes. There is one non-recursive watch per library root.

## Detection and scanning

`library::Detector` scores the executables in one game folder using the `detect.*` settings. Each rule in `detect.rules` is a plain function run in the listed order, and removing a rule from the list disables it. Windows candidates are `.exe` and `.msi` files; Wine runs an `.msi` through `msiexec` and a `.bat` or `.cmd` through `cmd`. A candidate can be flagged `is_installer` by name (`detect.installer_name_patterns`) plus size, and an `.msi` always is. A game whose best candidate is an installer is stored `needs_install`. The default deny and installer patterns live in `src/config/KnownExePatterns.h`.

`library::Scanner` treats each folder directly under a library root as one game. A folder already known by `install_path` is never detected again, so a scan never overwrites a user's changes. `prefix_root` and anything that looks like a Wine prefix are skipped. Folders that disappear are marked `missing`, or removed when `library.remove_missing` is on.

`library::AutoSetup` stores a new game and publishes `game.added` before any provisioning. Native games are stored `ready`, Windows games `setting_up`.

`library::Watcher` runs `Scanner::ScanRoot` when a root changes. A new folder is scanned once its size stops changing for `scan.debounce_ms`. A deletion is scanned right away.

## Runners

`runner::IRunner` has `kind()`, `Discover(config)`, `Provision(game, build)` and `BuildCommand(game, build)`. There are four runners:

- `NativeRunner` runs the executable directly.
- `ProtonRunner` runs Proton builds through `umu-run`. It uses the `umu-run` on `PATH`, or Mira's own copy under the tools folder.
- `WineRunner` runs the system Wine or any Wine build it finds.
- `SteamRunner` runs a Steam game with the Proton build and prefix Steam already set up for it.

Neither `umu-run` nor `wineboot` gives a reliable exit code for prefix creation, so both runners check that `drive_c` exists afterwards.

`RunnerRegistry` turns a `kind:name` reference into a runner and build. `latest` picks the newest build. `auto` (the default for `default_runner.windows`) prefers a distro-packaged build, then GE-Proton or Wine staging-tkg, then the newest, and uses Proton when any Proton build is installed. Provisioning pins the resolved build onto the game's `runner_ref`, so a later runner update never changes a working game.

Windows games are provisioned when they are detected and again on later scans while they are still `setting_up`. A game that broke because no runner was available is provisioned again on each scan, when a runner download finishes, and when it is launched.

`runner::Downloader` lists and installs Proton and Wine builds from GitHub releases (sources in `src/config/RunnerSources.h`) and checks their checksums when the release has them.

## Launching

A direct launch goes through `mira-run` (`src/wrapper/main.cpp`). `mirad` resolves the runner, applies `command_wrappers` and `launch.env`, and passes the command to `mira-run`. It waits only for a short status handshake over a pipe.

`mira-run` owns the session: `launch.pre_script`, the game process, `launch.post_script`, the game's output in `logs/<id>.log`, and a session record in `sessions/` written before and after. The session completes even if `mirad` dies in the meantime. `launch.gamemode` registers the game with GameMode over D-Bus through `gdbus`.

`mira-run` shares its process group with the game, so `kill(-pid)` stops the whole tree. It ignores SIGTERM and SIGINT so it can still run the post script and write the final record.

`proc::ProcessSupervisor` waits on `mira-run` and reads the finished session record to decide between a crash and a clean exit. At startup, `Reconcile()` archives finished sessions, re-adopts any `mira-run` still alive and closes the rest as `incomplete`. A finished session is added to the game's `play_seconds` once: the game remembers the newest session it counted (`last_session_at`).

If `mira-run` can't be found or started, `mirad` launches the game directly with no session record or log.

## Desktop entries

`desktop::DesktopEntries` writes a `mira-<id>.desktop` entry for each ready game and keeps them in sync after scans and game changes. `desktop_entries.*` settings are resolved per game, so a game can opt out. `Exec=` always goes through `mira launch <id>` (or the GUI), never the game's executable, so playtime and crash tracking always apply. It only touches files it created.

## Planned

- A `winetricks_defaults` setting that runs baseline verbs on every new prefix.
- Reading Steam's `appinfo.vdf` to fill in the launch executable for `steam.launch_mode: "direct"`.
- `mirad --scan-once`: scan, provision, print a summary and exit.
- An endpoint for per-session play history.
- Steam achievements through the Steam Web API, when a key is configured.
- Provisioning EOS and EasyAntiCheat for Epic games that need them.
