# The frontend (`mira-gui`)

`mira-gui` is a Qt6 client of the REST API in [`api.md`](api.md). It never links against `mira_core`.

## UX principles

These hold across every screen; a new one follows them without being told.

- **Nothing moves on its own.** A control that comes and goes keeps its space while hidden (`setRetainSizeWhenHidden`), so revealing it never shifts a row, a header or a card. A header is the same height with or without its buttons.
- **No permanent help chrome.** Help lives on the thing it explains: a setting's doc is its label's tooltip after 0.3 s of hover, not a [?] icon. Labels say what the control does, so most never need the tooltip.
- **Undo before reset.** A changed value shows a dot and an undo button that stay until it's saved or undone; undo puts back the saved value. Reset to defaults is a card-level action, offered only where the card's values belong together (layout, shortcuts, detection, scanning and the like) and only while something in it isn't default. Neither applies before Save.
- **Line up with the structure.** Popups anchored to a card start at the card's left edge (6 px in) rather than centering under whatever was hovered.
- **Every interactive state is styled.** No Fusion bevels or native hovers: frameless icon buttons share one flat rounded hover (`QToolButton[autoRaise="true"]` in `base.qss`), and a new button kind gets its hover, pressed, checked and focus looks together.
- **Previews are never blank.** With no games to show, a preview draws sample cards in the theme's own colors (`theme::SampleArt`), so the setting still shows what it changes.
- **Quiet by default.** Secondary actions are muted text or icons until hovered; nothing explains itself in a subtitle or hint line.

## Windows

`mira-gui` opens on the **grid** (`window/LibraryWindow`): cover tiles, a left sidebar and a custom top bar (`window/TopBar`) in place of a native titlebar. The window is frameless. The top bar carries the Mira badge and name; dragging its empty area moves the window, double-clicking the top bar toggles maximize, and the edges resize. Moves and resizes go through `QWindow::startSystemMove`/`startSystemResize` so they work on Wayland and X11.

### Library page

The grid page (`library/LibraryPage`) opens on the same tab row as a source page (`widgets/TabRow`): tabs for All, Installed, Playing now, Needs attention and Never played, then the filter and sort menu (`library/FilterSortPill`) and the search box. When the window narrows, the search box gives way first (320 px down to 160, then a search button that opens it again, as does Ctrl+F), then the last tabs move into a More menu; the current tab always stays, and source pages' rows do the same. There's no page title; the sidebar says where you are. Under them, *Continue playing* (`library/ContinueRow`) shows large cards for running and recently played games while the whole library is shown. Tiles show a status line, which also names the source a game came from: after the status ("Playing · Steam"), or alone and muted when there is none. The search box matches names and tags (a plain word finds either, `#word` only tags), and the filter menu's Tags section lists every user tag as a pill with its count under the current filter, in the library's tag order, two to a row with equal widths (the popover widens up to 360 px to fit the names, then cuts them short); picked tags all have to match, show after the filter's name on the pill, and never bring hidden games outside Hidden. A middle click on the pill, or a click on the sidebar's Library while the library is already shown, goes back to all games with no tags picked and an empty search. A tag chip's menu in a game's card has *Show games tagged …*. A game with `needs_check` (Mira wasn't sure which program starts it) reads *Not checked* on its status line and is listed under Needs attention; its card marks the Executable row and offers *Looks right*, and saving any change also clears it. Each part can be turned off in Settings → Interface.

### Sidebar

From top to bottom: the Mira header, the Library, Runners and Settings rows, pinned games, the sources, recently played games (off by default), and the Add games button.

Pinning a game (game menu, or several at once from the batch menu, which offers Pin/Unpin and Hide/Unhide for whichever selected games each would change) adds the `favorite` tag, the same one Lutris imports its favorites under. Pinned games list by name under PINNED; their tiles carry no mark. PINNED follows the grid's filter: hidden pinned games show only under the Hidden filter, and only they do. Clicking a pinned or recently played row plays the game; the second click of a double click is ignored, since the list can reorder under it. Source rows lead with up to three of the source's covers fanned like a hand of cards (most recently played on top, hidden games left out; a dashed outline when it has none), then the name over a muted line: how many games it has, *No games*, or *Signed out* in orange for a store that has games but is signed out. With covers turned off, a row is one line with a colored dot and the count.

Only sources that are set up show in the sidebar. *Manage sources* (`sources/ManageSourcesCard`) is an in-window card over the content, sharing the sidebar style card's overlay so the sidebar stays bright beside it. It lists every source in sidebar order as one freely reorderable list (drag the grip or Alt+Up/Down), each row with its kind tag and status inline, a switch that turns the source on or off, Set up for one that isn't set up yet, and a ⋯ menu for open, import, show in sidebar and remove. Every change applies at once. `sources/Sources` lists them for every view.

### Source pages

Each source row opens `sources/SourcePage` in the grid's place:

- **Stores** (Epic Games, GOG, itch.io, Amazon Games, Humble Bundle): install the helper tool, sign in by pasting what the login page shows, import installed games. Owned games that aren't installed show as a second grid with an Install button (Download for Humble). A downloaded Humble bundle's button becomes *Add to library…*, which opens the add-game dialog on its download folder; a bundle with nothing to download (such as a Steam key) says so.
- **Launchers** (Battle.net, Ubisoft Connect, EA app): install the launcher into its own prefix, open it, import its games.
- **Local** (Steam, Lutris): import what the other program installed. Steam also lists owned games once a Web API key is set.

A page opens on a tab row that also holds the source's status and its Sign out / Open, Import, settings and ⋯ buttons, a "Set up <source>" card while a step is left (one row per numbered step, the current one bold and holding its explanation and accent button), and the source's games as tiles (`library/TileGrid`), split into Installed and Not installed tabs unless `source_page_tabs` is off. The zoom slider sizes each page on its own unless `tile_size_synced` is on. Covers for games that aren't installed come from `/v1/library/artwork`. Login URLs and paste parsing come from `mirad`, so the page only holds wording. A source turned off with `<id>.enabled` isn't listed.

The banner's gear opens `sources/SourceSettingsCard` under it: the source's runner (`/v1/sources/{id}/runner`) and every schema setting under its own keys (`<id>.*`, plus `launchers.<id>.*`), in the same rows as Settings and saved or discarded together from the card's foot. The banner's ⋯ menu updates a store's tool, opens a launcher's prefix tools (folder, winetricks, run a program, log) and removes the source through the same confirmation Manage sources uses.

### Runners page

`runners/RunnersPage` has a Proton/Wine switch that filters both of its lists. They are settings cards in two columns, which stack into one on a window under 1150 px wide:

- **Installed** builds are one row each: the label, a Default tag on `default_runner.windows`, and how many games use it, its version and source inline (elided when narrow). Builds Mira downloaded show Update when there's a newer one and Remove in their ⋯ menu; distro and Steam builds show as managed outside Mira. After an update it offers to remove the old build. *Make default* (⋯) and *Pick automatically instead* (the note under the card) set `default_runner.windows`.
- **Get more** lists releases from the chosen source (the dropdown in its header), with date, size and checksum inline and an Install button on each.

A Missing tools card above Installed offers to install umu-launcher or winetricks when either is missing.

### Tags page

The sidebar's *Tags* row (between Runners and Settings) opens `tags/TagsPage`, settings cards from `GET /v1/tags` and the `tags.*` settings. The window builds it a moment after mirad connects and keeps it, following tag events while it's out of view, so it opens at once; leaving it closes an open picker unapplied and drops a question in its bar. Your tags and Tag settings share a left column only a little wider than their rows need; Tag settings sits at the bottom and Your tags takes all the height above it (at least 240 px), its rows scrolling inside with GAMES and FOLDER column headings pinned over them, so Tag settings is always in view. From Steam fills the rest, its rows in as many equal columns of at least 250 px as fit (`RowColumns`), read across, names cut short with an ellipsis; below 1000 px it goes under the left column. Each row's count is an accent pill, stronger the more games have the tag (log scale from two games to the most). A round plus button opens the picker, filling with the accent while its row is under the pointer. Rows are kept and updated in place as the tags change, so a switch flipped or a count changed never rebuilds the page.

- **Your tags**: folder tags first, in `tags.folders` order, which picks the folder of a game with several; their grips drag them into another order (or Alt+Up/Down), asked about in the change bar when that moves folders. Then the other tags, most games first. Each row has its count, a Folder switch, which makes it a folder tag in every library folder sorted by tag, and a ⋯ menu (*Choose games…*, *Rename…* in place, *Remove from every game…*). A rename asks in the change bar when it moves folders, and always before a merge into a tag the library already has. *New tag* in the header.
- **From Steam**: the Steam tags on two or more of the library's games that aren't its tags, most games first, with a search and a plus button on each. The tags on one game each (most of Steam's) show only from a button under the list or by searching. Steam tags never show anywhere else. Games whose metadata predates Steam tags get them once a session, when the page is built (`POST /v1/tags/fetch`).
- **Tag settings**: a switch per library folder for sorting it by tag, and where tags come from.

Adding, *New tag* and *Choose games…* open `tags/TagPicker` in place of the cards: covers to tick, the games Steam gives the tag (or the games that have it) first and ticked, then every other game unticked, with a search, and a floating bar with an *Also a folder* switch, Cancel and *Add to N games*. The covers follow the library's tile size and zoom with it (Ctrl+wheel, the slider, Ctrl+=/-). Hidden games wait in a last *Hidden* section that Ctrl+H shows, ticked only if they have the tag; store launchers aren't offered and keep the tag as they had it. It sends `POST /v1/tags/set`, so afterwards exactly the ticked games (and those launchers) have the tag. A change that moves folders (the Folder switch, sorting a library folder) asks in the change bar first with how many move (`POST /v1/tags/preview`), and applies at once when none would; removing a tag asks there too.

### Activity

The top bar's download button opens `activity/DownloadsPanel`, titled Activity. It lists everything `activity/DownloadTracker` has seen from the event stream: game installers, store installs and updates, Humble downloads, launcher installs, tool downloads, runner downloads and `mirad`'s jobs (scans, imports, moving and deleting games, removing a source). `mirad` replays recent events on connect, so work started before the GUI opened also shows. Game installers report bytes written; Epic, GOG, Amazon and itch installs report percent, speed and time left (`library.install.progress`), also drawn on the title's tile in the library, on its source page and in search: the title, a rail (moving while no percentage is known), the percentage and time left, and the speed or bytes written under it (`DownloadTracker::TileProgressFor`); jobs with steps report how far along they are. A finished install offers *Show*, which selects the game.

When a launched game turns out to have been an installer (`game.install_detected`), `game/InstallPromptCard` asks over the library, in the same overlay as Manage sources: the program found (or *Choose…* one inside the installed folder), a switch to mark it as an app, and *Keep as is* or *Use this program* (`finish-install`). While Mira is hidden or minimized, a notification asks first and its *Review…* button opens the card.

When a scan finds a folder that could be any of several games moved by hand (`library.move_unclear`, and `GET /v1/library/unclear` on every connect), `game/UnclearMoveCard` asks in the same overlay which game it is ("Which game is ~/Games/Copied?", with each game's cover), or *It's a new game*; *Ask again later* leaves it waiting in mirad. `library.move_settled` (settled by another client, or the folder gone) takes the card away, shown or queued.

Two more cards share that overlay (`game/InstallerCards`). `InstallerCard` runs a game's installer: the file found (*Change…* picks another), then *Not now*, *Install quietly* when the format allows, and *Show the installer*. It opens from *Install…* and double-clicking a game that needs installing, and on its own when a new installer is found (`game.added` with `needs_install`), unless the scan already runs it (`auto_install`). `InstallerLeftoverCard` follows `game.installer_leftover`: the folder and its size, *Keep* (the default) or *Delete* (`DELETE /v1/games/{id}/installer`). Every such card waits its turn in one queue (`LibraryWindow::QueueCard`) and never covers Settings, a game's card or another card.

Jobs answer `202` at once (see [api.md](api.md#jobs)). `client/Jobs` waits for each one's `job.finished` or `job.failed` on the shared event connection, and after a reconnect asks `GET /v1/jobs/{id}` about any it was still waiting on, so the `client/api` calls that start them still hand their caller one result.

### Selection and hover

One click selects a tile. Ctrl, Shift or a drag selects several, and the context menu then offers batch actions; source pages work the same way (`widgets/TileView`), with *Install (N)* on not-installed titles. Double-click launches. A single click never launches, so a misclick can't start a game. *Drag to select* on the Interface tab turns drag selection off.

Hovering a tile shows `library/HoverCard` with the name, status, runner, ProtonDB tier, developer and genres. Tooltips everywhere go through `widgets/ToolTip`, which draws them as the same card.

### Keyboard

`app/KeyBindings` holds every shortcut, and each can be changed in Settings (stored in `shortcut_overrides`). `app/Shortcuts` installs `Ctrl+Q`, `Ctrl+W` and `F1` (list of shortcuts) on both windows. The grid adds `Ctrl+F` search, `Esc`, `Ctrl+1` to `Ctrl+9` filters, `F5`/`Ctrl+R` refresh, `Ctrl+,` settings, tile size keys and, while the grid has focus, `Enter` to play or stop, `Alt+Enter` for details and `Delete` to remove. That last group is scoped to the grid so the keys still work in the search box.

Quit goes through `QApplication::closeAllWindows()` so `LibraryWindow::closeEvent` saves its prefs.

## Theming

`theme/Theme` sets the Fusion style, palette and stylesheet at startup instead of inheriting the desktop's.

A theme is a TOML file of tokens. `themes/base.qss` is the only stylesheet, and every `@token` in it is filled from the current theme. `mira-dark` and `mira-light` are built in; user themes go in `$XDG_CONFIG_HOME/mira/themes/*.toml`. Missing or invalid keys fall back to the defaults. The `theme` pref is a theme name or `auto`, which follows the desktop's light/dark setting.

`theme::Overrides` applies tile spacing, grid padding and corner radii from `frontend.toml` on top of any theme. A key left out of the file uses the theme's value.

Widgets that paint by hand read tokens directly and repaint on `theme::Notifier::Changed`: `library/GameTileDelegate`, `library/CoverArt`, `app/Notify` and `game/HeroBackdrop`.

Widgets don't set colors themselves. They set a style property (`setProperty("role", "muted")`) and `base.qss` styles it; `theme::SetStyleProperty` re-polishes after a change. `theme/Icons` draws icons as vector paths in the theme's text color.

## Layers

```
client/    talks to mirad, draws nothing
theme/     the theme tokens, stylesheet and icons
widgets/   general-purpose widgets that know nothing about games, and the label, heading and divider helpers
app/       process-wide services: notifications, error help, shortcuts, tray, starting mirad
library/   the library model, the library page and how a game is drawn: tiles, covers, hover card, game menus and actions
game/      a game's card: its edit form, overrides, art picker and installer cards
sidebar/   the sidebar: nav rows, source rows, pinned and recently played rows and their style card
sources/   the source list, source pages with their setup and settings cards, removing a source, and Manage sources
runners/   the Runners page
tags/      the Tags page and its game picker
activity/  the download and job tracker and the Activity panel
settings/  the Settings screen and the card and row pieces every settings screen shares
window/    the main window, its top bar and frameless edge, and About
dialogs/   modal dialogs
```

`client/` and `theme/` depend on nothing else here, and `widgets/` only on `theme/`. The feature folders use those and each other, and `window/` puts them together.

| `client/` file | Role |
|---|---|
| `Transport` | One socket round trip: timeouts and the error envelope. |
| `JsonMapping` | JSON to and from the structs in `Types.h`. |
| `Async` | Runs a call on a worker thread and delivers the result on the main thread. |
| `api/` | One function per endpoint, in `namespace api`, one file per area: `Games`, `Library`, `Config`, `Artwork`, `Runners`, `Stores`. `Request` holds what they share: `ReadReply` turns a reply into a result, checking the body's shape and catching a field of the wrong type, and `RunJob` waits on a job. |
| `Jobs` | Waits for a job's `job.finished` or `job.failed`. |
| `EventStream` | The `GET /v1/events` connection, reconnecting with `Last-Event-ID`. |
| `EventHub` | Shares one event connection between every window. |
| `Events` | Parses each event's payload (`events::ParseGameState` and so on). |
| `Types.h` | Plain data the UI depends on. |

`async::Deliver` posts results to `qApp` and checks a `QPointer` to the requesting widget on the main thread. Posting to the widget itself would read a possibly deleted object on the worker thread.

`LibraryWindow` fetches the whole library once into `library/GameLibraryModel`, and `library/LibraryPage` filters it on the client, which keeps search instant and makes "Playing now" and "Never played" possible. Events then patch the view directly. There is no polling.

A search also looks through the games you own but haven't installed. `library/OwnedTitles` fetches every store's titles (`GET /v1/library`) in the background a few seconds after connecting, again when a typed search finds the list more than ten minutes old, and after a store install finishes. Matches (`MatchOwned`, one per game across stores by name) show under the library's own in a *Not installed* section with an Install button; a game owned on several stores asks which copy to install. While that section shows, the grid sizes to its tiles and the page scrolls as one. Their covers are fetched only for titles a search showed.

Only `game.added` and `game.updated` carry a game record, so views check the event type before parsing one. `game.state` carries only the state and launch details, so an exit triggers a re-fetch. A game handed to Steam or a launcher reports `tracked` in the launch reply and `game.launched`, and isn't marked running unless it is tracked.

## Game card

*Game settings* opens a card over the library (`game/GameCard`): the game's hero art with Back at the top left, *Change art* at the top right, and the cover, name, status and Play under them. Below sit `game/GameEditForm`'s Launch, Runner, Details and Files cards, built from the same rows as Settings, with tags as chips (`widgets/TagEdit`), always in the library's tag order (`TagOrder`, the Tags page's: folder tags in their order, then by how many games have each), whatever order the game stores; the filter's Tags list and the menus' *Tags* follow it too. In a library folder sorted by tag, the game's folder tag (its pick, `folder_tag`, else its first tag in the record's `folder_tags` order) is outlined with a folder icon; a store game, which never moves, gets no mark. Its other folder tags carry a muted folder icon; a click on one, or its menu's *Use as this game's folder*, picks it for this game (no pick when the order gives it anyway), and the folder chip's *Follow the folder tags' order* drops the pick. A click on any other chip does nothing, so a click only ever means a folder pick. The pick saves with the tags in one `PATCH /v1/games`. Which tags are folder tags, and their order, is set on the Tags page only. The room the change bar needs under the last card stays once added, so a card scrolled to the bottom never jumps as the bar hides. The library menus' *Tags* lists every tag in the library for the game or the selection (a partly shared one as "RPG (2 of 5)", adding it to the rest; a fully shared one checked, taking it off) with a field for a new one. The folder tags of the games' library folders come first under a *Folders* heading row (a disabled row, since Fusion draws a checked action's icon in place of its tick and a menu section's text not at all). *Advanced settings* swaps the cards for the runner options and the game's overrides of global settings (`game/OverridesEditor`). Runner options are one row per key of the picked runner's `GET /v1/runners/{kind}/schema` (for "Default runner", the kind of the record's `default_runner`), titled by the kind and hidden when it has none; environment variables are name and value rows (`widgets/KeyValueEdit`). Both save as merge patches of the keys that changed, a cleared one as null; Back and Esc step out of it, then out of the card. The same change bar counts unsaved edits from both and saves them through `PATCH /v1/games/{id}`, `PATCH /v1/games` for tags and `PATCH .../config`. The card stays open after a save.

## Settings

Settings takes over the window body, its nav column in the sidebar's place: the sidebar's current width (plus the splitter handle, so the divider doesn't move), its margins, row height and color, and its back row as tall as the sidebar's first row. `settings/SettingsNav` lists the categories under three headings (Look and feel, Games, System), each with an icon, and shows every category's page of cards (`settings/SettingsCard`) one after another in a single scroll. Clicking a category scrolls to its page, the list follows the scroll, and the current page's title sticks to the top; the last page gets room below it so it can reach the top too. A page lays its cards in as many 600 to 760 px columns as fit (`settings/SettingsColumns`), using fewer when that's nearly as short, and may move a card to another column to even the columns out; each column keeps the cards' order, and the last card of a shorter column stretches to the common bottom unless it folds. The split is kept until the width or the shown cards change, so folding a card never moves others between columns. In the scroll, the wheel only changes a dropdown, number box or slider once it has focus. Every page that scrolls (Settings, a source page, Runners, a game's card, Activity, the sidebar) goes through `widgets/Scrolling`: a wheel notch moves about 120 px, and Page Up/Down and Home/End scroll the page wherever focus is in it, the nav column included, except in a number box or text area. A game's own settings (`game/OverridesEditor`) use the same scroll. Every screen that edits settings is built from the same pieces: a titled card, rows with the label on the left and the control on the right (a switch for on/off), and a line between rows. A row's doc is its label's tooltip, shown after 0.3 s of hover. While a row has a change that isn't saved yet, a dot marks it and an undo button puts back the saved value. Cards whose settings make sense to reset together (Layout, the Shortcuts cards, and schema groups with `group_resettable`) get a "Reset to defaults" button in their header while anything in them differs from its default; like any edit, it applies on Save. Nothing applies until saved: the change bar at the bottom counts the changes and saves or discards them, and Back asks first if anything is unsaved. A setting put back to its default is removed from `settings.toml` (`POST /v1/config/reset`) rather than written out.

- **`settings.toml`** holds backend settings. Their pages are generated from `GET /v1/config/schema`: categories, order, labels, card titles (`group_label`), folded cards (`group_collapsed`), card resets (`group_resettable`), runner pickers (`is_runner_ref`, showing a listed runner by name and storing its reference, and listing builds again when one is installed or removed, from `EventHub::RunnersChanged`), folder and file pickers (`path`) and lists for arrays (`widgets/ListEdit`) all come from the schema, so a new backend setting needs no frontend change. A category whose every setting has a `source` (Sources) shows one folding card per source, with the source's on switch in its header. Each row's editor comes from `settings/SettingEditor`, which a game's own settings (`game/OverridesEditor`) and the source settings card share. A secret is masked, with a show button inside the field, and a `link` is a button beside it. The schema's Tags category is left out: the Tags page shows those settings. Search (`settings/SettingsSearch`) matches every typed word in any order against the key, label, category, card title, doc and `keywords`, and opens a folded card that matches. It filters the scroll in place: pages with no match hide and dim in the list, and the rest show their match count.
- **Interface, Sidebar and Shortcuts** are the GUI's own pages. Themes are picked from tiles that draw a small window in each theme's colors (`settings/AppearancePreviews`, `theme::Peek`); the tile toggles sit under two of the user's games drawn by the grid's own `GameTileDelegate`; the layout sliders reshape a strip of covers before anything is saved. The Sidebar page holds the same rows as the in-window Pinned and recently played card (`sidebar/SidebarStyleCard`), and every source with a switch for showing it in the sidebar and a grip for dragging it to a new place (or Alt+Up/Down). Shortcuts are keycaps (`widgets/ShortcutEdit`): click one and press the new keys, Esc cancels and Backspace leaves none.
- **`frontend.toml`** holds GUI state and preferences (`FrontendPrefs` in `client/Types.h`), read and written as the `frontend` key of `/v1/config`. The backend never validates it.

| Key | Meaning |
|---|---|
| `window_width`, `window_height` | Window size. |
| `sidebar_width` | Sidebar width. |
| `tile_width` | The library's tile size, clamped to the zoom slider's range. |
| `source_tile_widths`, `tile_size_synced` | Each source page's tile size, or one size for every page. |
| `library_filter`, `sort_by`, `sort_descending` | Selected filter and sort. |
| `scan_on_startup` | Run a library scan when the GUI opens. |
| `theme` | Theme name or `auto`. |
| `drag_select` | Drag across the grid to select. |
| `double_click_play` | Double-clicking a game plays or stops it. |
| `tile_spacing`, `grid_margin`, `tile_radius`, `panel_radius`, `control_radius` | Shape overrides in pixels, `-1` for the theme's value. |
| `shortcut_overrides` | Changed shortcuts by id. |
| `hidden_sources`, `source_order` | Which sources the sidebar shows, and in what order. |
| `source_imported_at` | When each source last imported. |
| `sidebar_recent_count`, `sidebar_source_counts` | How many recently played games to list (0 hides them), and whether source rows show counts. |
| `sidebar_source_covers` | Fanned covers on two-line source rows; off shows one line with a colored dot instead. |
| `sidebar_pinned_style`, `sidebar_recent_style`, `sidebar_recent_when` | How PINNED and RECENTLY PLAYED draw their games (`covers`, the default, `hero` or `shelf`), and whether recent rows say when each was played. |
| `library_filter_tabs`, `library_continue_row`, `library_continue_count` | The library's tabs and its Continue playing cards. |
| `tile_status`, `tile_source_mark` | What a tile's status line shows: the status, and the game's source. |
| `source_page_tabs` | Installed and Not installed as tabs on source pages. |

Every field is optional; a missing one means the default. Values go through the widget that owns them, so a hand-edited value is still clamped.

## Notifications

`app/Notify` handles every message. Failures (`Failed`, `FailedWithHint`, `FailedWithAction`, `Warn`) are persistent desktop notifications; confirmations (`Notice`) use the desktop's default timeout. Only questions (`Confirm`, `LeaveUnsaved`) and answers to a button (`Info`) are popups. Pages with their own status line, such as `RunnersPage` and `ArtPickerPanel`, report errors there.

Success that already shows on screen gets no message. A notice only goes out when nothing in the window would change, such as "No new Steam games found."

Notifications use `org.freedesktop.Notifications` (`app/SystemNotifier`) with a `desktop-entry` hint of `mira`. `FailedWithAction` adds a button that raises the window and runs the action. Without a notification service, failures become popups and notices become cards in the window's corner.

Messages are plain text, since `mirad`'s errors quote paths and commands. Failures show `mirad`'s message under a sentence naming what failed.

Every request result carries an `ApiError`: `mirad`'s message, code, `hint` and `fix` (see the error envelope in [`api.md`](api.md)). `notify::FailedRequest` shows the hint under the message and turns the fix into a button through `app/ErrorHelp`: the setting to fill in, the Runners page, the game's settings or log, running its installer with the window shown, or the store's page. What to say and where to point is `mirad`'s knowledge, so a new error needs no frontend change. The one case `ErrorHelp` words itself is `mirad_unreachable`, set by the transport when a request never reaches `mirad`, which gets a *Start mirad* button. Status lines use `error_help::Describe` for the same text inline. Failure events carry the same fields, so an Activity row for a failed install or download shows the hint and a button for the fix. `LibraryWindow` registers the routes with `error_help::SetNavigator`.

A `crashed` `game.state` gets a failure with a *View log* button, titled by its `code`: "couldn't start", "was killed" or "crashed". `mirad` only reports one for a real crash, so a game that exits non-zero on a normal quit stays quiet.

On connect, `mirad` replays its event buffer and then sends `stream.live`. Windows apply replayed events but only announce (notifications, install results, crashes) what comes after it, so a restart doesn't repeat old messages.

## Cover art

`library/ArtworkStore` fetches art from `GET /v1/games/{id}/artwork` and always returns a pixmap:

- A game's record says which art it has (`art`, see [api.md](api.md)). A game without a cover is never asked for one; `library/CoverArt` draws a placeholder with a hue from the game's id, so it stays the same across restarts.
- A cover is fetched once, and again only when its version changes in a record, `game.metadata_ready` or `game.artwork_selected`. The old image stays on screen until the new one lands.
- At most eight requests are in flight.
- The original image is kept and scaled on demand for the zoom slider.

Without `steamgriddb.api_key`, non-Steam games may have no source. `mirad` then fails the fetch with `no_steamgriddb_key`, and the GUI says so once per session: a popup offering Settings when the user asked, a notice after a background scan.

Metadata is only fetched when a game is first added. A tile's *Refresh metadata && cover art* and *Fetch missing cover art* ask again.

`game/ArtPickerPanel` is the game card's *Change art*, with a *Covers* and a *Hero art* tab. It shows a slot's candidates as a grid of previews with style filters, loading SteamGridDB pages as the grid scrolls. `mirad` downloads the previews (`POST .../artwork/thumbs`) only for what is on screen plus one screen ahead, and deletes them when the GUI quits. Clicking a preview shows it on the card and in the change bar; *Use this cover* (or hero) applies it and *Cancel* goes back to the one in use. The *Art from* menu picks which SteamGridDB game the art comes from.

## API use

| Endpoints | Where |
|---|---|
| `GET /v1/health` | Online/Offline badge |
| `GET /v1/games`, `GET`/`PATCH /v1/games/{id}` | Library views, `game/GameEditForm` |
| `DELETE /v1/games/{id}` | `DeleteGameDialog` |
| `GET`/`PATCH /v1/games/{id}/config` | `OverridesEditor` |
| `POST /v1/games/{id}/launch`, `/stop` | Play/Stop, double-click, context menu |
| `POST /v1/games/manual` | `AddManualGameDialog` |
| `POST /v1/games/{id}/run` | `RunInPrefixDialog` |
| `GET /v1/games/{id}/installer`, `POST .../install`, `GET .../install/progress`, `POST .../finish-install`, `DELETE .../installer` | `game/InstallerCards`, *Mark as installed*, `game/InstallPromptCard`, `DownloadTracker` |
| `POST /v1/games/{id}/tricks` | `WinetricksDialog` |
| `GET /v1/logs/{channel}` | `LogWindow` (a window per log), the setup card's running tail, the Activity popover's *Log* buttons |
| `POST /v1/games/{id}/relocate`, `/v1/library/relocate` | *Move to Mira's folders…*, and the game card's *Move…* buttons for the install folder and the prefix |
| `/v1/tags`, `/v1/tags/*` | `tags/TagsPage`, `tags/TagPicker` |
| `GET`/`POST /v1/library/unclear` | `game/UnclearMoveCard` |
| Metadata and artwork endpoints | `ArtworkStore`, `HoverCard`, `GameDetailPageDialog`, `ArtPickerPanel` |
| `POST /v1/library/scan` | Startup and *Refresh library* |
| `/v1/library`, `/v1/library/install`, `/update`, `/artwork` | `SourcePage` |
| `/v1/{epic,gog,itch,amazon,humble}/*`, `/v1/launchers/*`, `/v1/sources/*` | `SourcePage`, `ItchCollectionsDialog` |
| `POST /v1/steam/scan`, `/v1/lutris/import` | Steam and Lutris pages |
| `/v1/desktop-entries/*` | `DesktopEntryImportDialog`, Settings |
| `/v1/config`, `/v1/config/schema`, `/reset` | `SettingsPanel`, `OverridesEditor`, `RunnersPage` |
| `/v1/runners/*` | `RunnersPage`, runner pickers |
| `GET /v1/gamemode/status` | Settings |
| `GET /v1/events` | `EventStream` |

## Running against a dev daemon

Run a separate daemon on its own socket and point the GUI at it:

```sh
XDG_CONFIG_HOME=~/.mira-dev mirad --socket ~/.mira-dev/mirad.sock &
MIRA_SOCKET=~/.mira-dev/mirad.sock build/dev/frontend/mira-gui
```

`XDG_CONFIG_HOME` only separates config, not the socket, so without `--socket` the dev daemon would take over the default one. The window footer shows which socket is in use.
