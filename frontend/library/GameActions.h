#pragma once

#include <QString>

#include <functional>
#include <string>
#include <utility>
#include <vector>

class QWidget;

// The things both library views do to a game, with the prompts and error
// reporting that go with them.
//
// The grid and the table used to carry byte-identical copies of launch,
// stop and delete, differing only in what they did afterwards. That
// "afterwards" is the `on_done` callback here; everything before it now
// exists once.
namespace mira_gui::actions {

// POST /v1/games/{id}/launch. `on_launched(tracked)` runs only on success.
// `tracked` is false when mirad handed the game to Steam rather than
// spawning it: no game.state event is coming, so a caller must not record
// it as running.
void Launch(QWidget* parent, const std::string& id, std::function<void(bool tracked)> on_launched,
            std::function<void()> on_failed = {});

// POST /v1/games/{id}/stop. Nothing to do on success: the `game.state` event
// that follows is what actually updates the view.
void Stop(QWidget* parent, const std::string& id);

// Asks (see DeleteGameDialog, which offers deleting the files and the
// prefix too), then DELETEs. `on_deleted` runs only if something was removed.
void Delete(QWidget* parent, const std::string& id, const QString& name,
            std::function<void()> on_deleted);

// Runs an executable inside the game's own prefix, asking which one first.
// Takes install_path/name directly, since both are already on GameSummary, so no
// fetch is needed just to open the dialog.
void RunInPrefix(QWidget* parent, const std::string& id, const std::string& install_path,
                 const QString& name);

// Flips a needs_install game to ready. 409s while exe_path is still empty,
// the normal case until the user points it at the installed program, so
// the failure message matters more here than elsewhere.
void FinishInstall(QWidget* parent, const std::string& id, std::function<void()> on_finished);

// Asks, then moves each game's files and prefix into Mira's own layout.
// `on_done` runs once every move has answered, whatever the outcome.
void Relocate(QWidget* parent, const std::vector<std::pair<std::string, QString>>& games,
              std::function<void()> on_done);

// Opens install_path in the desktop's file manager. Takes the path directly
// rather than an id: GameSummary already carries it, so no fetch is needed.
void OpenInstallFolder(QWidget* parent, const std::string& install_path);

// Opens the game's live log in a window of its own. Unlike RunInPrefix/Delete, no detail
// fetch is needed first: id/name are already known from the tile/row.
void ViewLog(QWidget* parent, const std::string& id, const QString& name);

// Opens WinetricksDialog. Fetches the full record first to check data_dir
// (not in the list summary) before opening; mirad only rejects "no
// prefix" asynchronously, so this catches it up front instead.
void RunWinetricks(QWidget* parent, const std::string& id, const QString& name);

// Flips this game's desktop_entries.enabled override to the opposite of
// `currently_enabled` (the caller already resolved it to label the menu
// item "Add"/"Remove", so this doesn't re-fetch).
void ToggleDesktopEntry(QWidget* parent, const std::string& id, bool currently_enabled);

// Confirms once for the whole batch (see AskDeleteGames), then DELETEs each
// game: file/prefix flags are skipped per-game for a desktop-entry source,
// same rule as the single-game Delete, checked with one GetGameAsync per
// game first since `source` isn't on GameSummary. `on_done` runs once, after
// every request settles.
void BatchDelete(QWidget* parent, const std::vector<std::pair<std::string, QString>>& games,
                 std::function<void()> on_done);

// PATCHes desktop_entries.enabled = `enabled` for every id. Unlike
// ToggleDesktopEntry this doesn't flip each game's own current value: a
// batch is an explicit "set them all to X" from a submenu, not a toggle.
void BatchSetDesktopEntry(QWidget* parent, const std::vector<std::string>& ids, bool enabled);

}  // namespace mira_gui::actions
