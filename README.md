# Mira

A Linux game launcher with one library for native games, Windows games run through Wine/Proton, and games from Steam and other stores.

Drop a game folder into a watched directory and Mira detects it, works out how to run it, and sets it up.

The repository has three programs: the daemon `mirad`, the CLI `mira` and the Qt frontend `mira-gui`. Both clients talk to `mirad` over a REST API on a Unix socket ([`docs/api.md`](docs/api.md)). See [`docs/architecture.md`](docs/architecture.md) for how they fit together and [`docs/frontend.md`](docs/frontend.md) for the GUI.

## Building

```sh
cmake --preset dev
cmake --build build/dev
```

Needs a C++23 compiler, CMake 3.20+, Ninja, and Qt6 6.5+ (`Widgets`, `Multimedia`) for `mira-gui`. Other dependencies are vendored. Binaries land in `build/dev/`. Other presets are `release`, `asan` and `tsan`. `cmake --build build/dev --target run-gui` builds and starts the GUI.

## Running

```sh
build/dev/mira-gui
```

`mira-gui` starts `mirad` itself and keeps it running from the tray. A `mira` command with Mira closed opens it in the tray first (or, with no desktop, starts `mirad` for that command only):

```sh
build/dev/mira list
```

[`docs/cli.md`](docs/cli.md) lists every `mira` command.

### AppImage, .deb and .rpm

```sh
cmake --preset release
cmake --build build/release --target appimage
```

Builds in an Ubuntu 22.04 container (docker or podman; nothing else is needed on the host, not even Qt), so the results run on Ubuntu 22.04 and newer. `build/release/appimage/` gets `Mira-x86_64.AppImage`, the `.deb` and the `.rpm`. The first run builds the image with GCC 13 and Qt 6.5 (`packaging/appimage/Dockerfile`), which takes a while; later runs reuse it and its compiler cache.

`--target appimage-host` (and `packages-host`) builds with the machine's own Qt and libraries instead: quicker, but the result only runs on distributions as new as that machine, so it's not for releases. It needs Qt6 and `curl`. Arch users can use `packaging/PKGBUILD` instead.

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
