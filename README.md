# Mira

A Linux game launcher with one library for native games, Windows games run through Wine/Proton, and games from Steam and other stores.

Drop a game folder into a watched directory and Mira detects it, works out how to run it, and sets it up.

The repository has three programs: the daemon `mirad`, the CLI `mira` and the Qt frontend `mira-gui`. Both clients talk to `mirad` over a REST API on a Unix socket ([`docs/api.md`](docs/api.md)). See [`docs/architecture.md`](docs/architecture.md) for how they fit together and [`docs/frontend.md`](docs/frontend.md) for the GUI.

## Building

```sh
cmake --preset dev
cmake --build build/dev
```

Needs a C++23 compiler, CMake 3.20+, Ninja, and Qt6 6.5+ (`Widgets`) for `mira-gui`. Other dependencies are vendored. Binaries land in `build/dev/`. Other presets are `release`, `asan` and `tsan`. `cmake --build build/dev --target run-gui` builds and starts the GUI.

## Running

```sh
build/dev/mirad &
build/dev/mira status
build/dev/mira list
```

Or run it as a user service:

```sh
systemctl --user enable --now mirad.service
```

[`docs/cli.md`](docs/cli.md) lists every `mira` command.

### AppImage

```sh
cmake --preset release
cmake --build build/release --target appimage
```

Produces `build/release/Mira-x86_64.AppImage` with `mirad`, `mira` and `mira-gui`. Needs Qt6 and `curl`. `linuxdeploy` and its Qt plugin are downloaded into `build/release/appimage-tools/` on first use. Arch users can use `packaging/PKGBUILD` instead.

That AppImage bundles the build machine's libraries, so it only runs on a distribution as new as that machine. For one that runs on Ubuntu 22.04 and newer, build it in a container (docker or podman, nothing else needed):

```sh
cmake --build build/release --target appimage-container
```

The result is `build/release/appimage-container/Mira-x86_64.AppImage`. The first run builds an Ubuntu 22.04 image with GCC 13 and Qt 6.5 (`packaging/appimage/Dockerfile`), which takes a while.

## Data

Everything lives in `~/.config/mira/` (`$XDG_CONFIG_HOME/mira`): `settings.toml` (backend settings), `mira.db` (the library, an SQLite database, with `mira.db.bak` as a copy from the last start) and `frontend.toml` (GUI settings). Back that directory up to keep all state; delete it to start fresh.

## Tests

```sh
cmake --build build/dev --target mira_tests
ctest --test-dir build/dev
```

Run the suite under the `tsan` preset for changes to anything shared across threads (`Config`, `GameStore`, `EventBus`).

## License

[GPLv3](LICENSE).
