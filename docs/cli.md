# `mira` command-line client

`mira` is a REST client over the same Unix socket and API the GUI uses (see [`api.md`](api.md)). It never reads `settings.toml` or the library database itself. Every command except `daemon` and `setup` needs a running `mirad`.

```sh
mirad &
mira status
```

A failed command prints `mirad`'s message, then its hint and the command that fixes it when the error carries one:

```text
mira: searching SteamGridDB needs an API key (HTTP 502)
      Add a free SteamGridDB API key. Steam games don't need one.
      Try: mira config set steamgriddb.api_key <value>
```

## Daemon and install

### `mira status`
`GET /v1/health`. Says whether `mirad` is reachable, or which socket it tried.

### `mira daemon [args...]`
Execs `mirad` (next to `mira` or on `PATH`) with the given arguments.

### `mira setup [--enable-service] [--remove|--uninstall]`
Run from the AppImage (`./Mira-x86_64.AppImage setup`). Writes a `~/.local/bin/mira` wrapper with `mirad` and `mira-run` links, a desktop entry with an "Uninstall Mira" action, the icon and a systemd user unit. `--enable-service` also enables the unit. A running `mirad` then points the games' menu entries at the wrapper. `--remove` undoes all of it, games' menu entries included, and `--uninstall` also deletes the AppImage after asking. Games, settings and prefixes are never touched.

### `mira watch`
Tails `GET /v1/events` and prints each event as it arrives.

## Library

### `mira scan`
`POST /v1/library/scan`. Scans every library root now and prints `added: N  missing: N  restored: N`. The watcher only reacts to changes, so run this after adding a root that already has games in it.

### `mira list [--status S] [--tag T]`
`GET /v1/games`, optionally filtered by status (`setting_up`, `ready`, `broken`, `missing`, `needs_install`) or tag.

```
celeste                  ready      [unreviewed] Celeste
hollow-knight            setting_up              Hollow Knight  (windows)
```

`[unreviewed]` marks a game nobody has corrected since it was detected. Games tagged `hidden` are left out unless you pass `--tag hidden`.

### `mira show <id> [--effective]`
`GET /v1/games/{id}` as JSON. With `--effective`, `GET /v1/games/{id}/config` instead: every setting as it resolves for this game, with the layer that supplied it.

### `mira set <id> [flags...]`
Changes a game. Game fields and setting overrides go to separate endpoints:

| Flag | Endpoint |
|---|---|
| `--name`, `--exe`, `--args`, `--runner kind:name`, `--data-dir`, `--env KEY=VALUE`, `--tag NAME`, `--untag NAME` | `PATCH /v1/games/{id}` |
| `--override dotted.key=value`, `--unset dotted.key` | `PATCH /v1/games/{id}/config` |

`--env`, `--tag`, `--untag`, `--override` and `--unset` can repeat.

```sh
mira set celeste --exe Celeste.exe --runner wine:system
mira set celeste --override scan.max_depth=8
mira set celeste --unset scan.max_depth
mira set celeste --tag hidden
```

`--tag` and `--untag` edit the current tags rather than replacing them. Override values are parsed as JSON and fall back to a plain string. Changing a game field marks the game reviewed; overrides don't.

### `mira add <install_path> <exe_path> [--name N] [--platform windows|native] [--installer]`
`POST /v1/games/manual`. Adds a game from anywhere on disk. With `--installer` it is stored `needs_install`.

### `mira remove <id> [--delete-files] [--delete-prefix] [--delete-metadata] [--purge]`
`DELETE /v1/games/{id}`. Without flags only the library entry is removed. `--purge` does all three deletes. Files are only deleted when they are inside a library root or `prefix_root`.

### `mira relocate <id>` / `mira library relocate`
`POST /v1/games/{id}/relocate` / `POST /v1/library/relocate`. Moves a game's files and prefix into Mira's layout, named per `prefix_naming`.

## Playing

### `mira launch <id> [file...]` / `mira stop <id>`
`POST /v1/games/{id}/launch` / `stop`. A Steam game under the default `steam.launch_mode` is started through `steam -silent -applaunch <appid>` and isn't tracked. A game left broken by a missing runner is provisioned again first. Files are passed to a Microsoft 365 app to open.

### `mira run <id> --exe PATH [--args ARGS]`
`POST /v1/games/{id}/run`. Runs any executable in the game's prefix, creating the prefix first if needed. This is the manual way to run an installer:

```sh
mira run my-game --exe UplayInstaller.exe
mira set my-game --exe MyGame.exe
mira finish-install my-game
```

### `mira install <id> [--interactive] [--installer PATH]`
`POST /v1/games/{id}/install`. Runs a `needs_install` game's installer and marks it ready once the game executable is found. Inno Setup, NSIS and MSI installers run silently, anything else (or `--interactive`) is shown. `--installer` picks the installer by hand and also works for a `broken` game. `--info` shows the installer's path, size, format and silent arguments; `--progress` shows install progress.

### `mira finish-install <id>`
`POST /v1/games/{id}/finish-install`. Marks a `needs_install` or `broken` game ready once `exe_path` points at the installed game.

### `mira tricks <id> <verb>`
`POST /v1/games/{id}/tricks`. Runs a winetricks verb in the game's prefix in the background. Watch for `tricks.finished` or `tricks.failed`.

### `mira gamemode status`
`GET /v1/gamemode/status`. Whether GameMode is installed and whether its daemon is reachable.

## Stores

### `mira library [source]`
`GET /v1/library`. What each account owns, marked `[installed]` when Mira tracks it:

```
epic     e8bbb84be35640cda646233152ff3428  [installed]  Brotato
epic     d26da9e047e4440a80781f13a8b7c062               Botanicula
```

A source that isn't set up lists nothing. Steam needs `steam.web_api_key` and `steam.steamid64` to list games that aren't installed.

### `mira library install <source> <ref>` / `mira library update <source> <ref>`
`POST /v1/library/install` / `update`. `<ref>` is the id `mira library` prints. The download runs in the background; watch for `library.install.finished`. Steam installs are handed to the Steam client and show up on the next `mira steam scan`. Steam updates its own games.

### `mira steam scan`
`POST /v1/steam/scan`. Adds or updates installed Steam games and prints `added: N  updated: N`.

### `mira steam status online|invisible`
`POST /v1/steam/status`. Sets your Steam friends status. Fails while Steam isn't running.

### `mira lutris import`
`POST /v1/lutris/import`. Imports games from Lutris's database.

### `mira store [list]`
Lists the stores: `epic`, `gog`, `itch`, `amazon` and `humble`. Each wraps a command-line tool and shares one set of verbs, `mira store <id> <verb>` (see [Stores](api.md#stores)).

### `mira store <id> status|setup|login [credential]|logout|import`
- `status` (the default) shows whether the store's tool is installed and the account signed in.
- `setup` downloads the tool into `~/.config/mira/tools` and waits for it. Run it again to update.
- `login` prints the sign-in URL, then reads back what to paste: Epic's `authorizationCode` or the whole JSON the page shows; GOG's `code` or the whole redirect URL; the amazon.com URL Amazon's login ends on; an itch.io API key from [itch.io/user/settings/api-keys](https://itch.io/user/settings/api-keys); or Humble's `_simpleauth_sess` cookie. Pass the credential as an argument to skip the prompt.
- `logout` forgets the sign-in. Not for Humble.
- `import` adds what the tool reports as installed, with the store as its source. Not for Humble. GOG only looks under `gog.install_root` (default `~/.local/share/mira/gog`).

Epic wraps [Legendary](https://github.com/derrod/legendary), GOG [gogdl](https://github.com/Heroic-Games-Launcher/heroic-gogdl) (which needs `python3`), itch.io [butler](https://itch.io/docs/butler/) (`mirad` keeps one `butler daemon` connection open, so a change to `itch.butler_bin` needs a restart), Amazon [nile](https://github.com/imLinguin/nile) and Humble [humble-cli](https://github.com/smbl64/humble-cli). Install titles with `mira library install <source> <id>`.

### `mira store itch collections [list | add <link> | remove <id>]`
Manages the collections whose games show in `mira library itch`: your own plus any added by link. Free games from them can be installed.

### `mira store humble bundles | download <bundle-key> [items]`
Humble has no installs, only downloads, so it isn't part of `mira library`. `bundles` lists purchased bundles. `download` fetches items (humble-cli's `1,3,5-7` syntax) into `<humble.download_root>/<bundle_key>/` and waits. Add the result with `mira add`.

### `mira launcher list|install|import|open`
Battle.net (`battlenet`), Ubisoft Connect (`ubisoft`), the EA app (`ea`) and Microsoft 365 (`office`), each in its own prefix. Microsoft 365's apps (Word, Excel, ...) are imported as apps.

- `install <id>` sets up the prefix, installs the launcher and imports its games.
- `import <id>` imports games installed through the launcher since.
- `open <id> [--launch REF | --install REF]` opens the launcher, or asks it to launch or install a game.

### `mira desktop-entries list` / `import <id>...`
`GET /v1/desktop-entries/candidates` / `POST /v1/desktop-entries/import`. Lists installed `.desktop` entries and adds the chosen ones as games.

## Metadata

### `mira metadata <id> [--refresh | --matches [QUERY] | --wrong | --match N | --match-id ID]`
`GET /v1/games/{id}/metadata`. Prints the cached store info. `--refresh` fetches it again in the background. `--matches` lists SteamGridDB matches for the name, starred where the art comes from. `--wrong` moves to the next match, `--match N` picks one from the list (`0` is the top match) and `--match-id` takes a SteamGridDB id.

## Runners

| Command | Endpoint | What it does |
|---|---|---|
| `mira runners` | `GET /v1/runners` | Installed Proton and Wine builds. |
| `mira runners sources [proton\|wine]` | `GET /v1/runners/sources` | Where builds download from, preferred first. |
| `mira runners catalog [--kind K] [--source ID]` | `GET /v1/runners/catalog` | Releases from one source, marking installed ones. Defaults to Proton and its first source. |
| `mira runners download --kind K --tag TAG [--source ID]` | `POST /v1/runners/download` | Downloads and installs a release in the background. |
| `mira runners updates` | `GET /v1/runners/updates` | Installed builds with a newer release. |
| `mira runners update <kind:name>` | `POST /v1/runners/update` | Installs the newer release and moves games and the default onto it. The old build stays. |
| `mira runners remove <kind:name>` | `DELETE /v1/runners/{kind}:{name}` | Removes a build. Only builds inside the search paths can be removed. |
| `mira runners tools [install umu\|winetricks]` | `GET /v1/runners/tools` | Shows or installs umu-launcher and winetricks. |
| `mira runners schema <kind>` | `GET /v1/runners/{kind}/schema` | What `runner_config` accepts for that kind. |

## Settings

### `mira config get|set|list|reset`

- `get <key>` reads one setting by dotted key.
- `set <key> <value>` sets one, parsing the value as JSON with a string fallback.
- `list` prints every setting's key, type, scope and description.
- `reset [key]` resets one setting or all of them.

Keys under `frontend.` go to `frontend.toml` unvalidated, for example `mira config set frontend.theme dark`. `list` doesn't show them.
