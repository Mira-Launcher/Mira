#include "GameActions.h"

#include <QDesktopServices>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include <map>
#include <memory>
#include <utility>

#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../dialogs/DeleteGameDialog.h"
#include "../dialogs/LogViewerDialog.h"
#include "../dialogs/RunInPrefixDialog.h"
#include "../dialogs/WinetricksDialog.h"
#include "../app/Notify.h"

namespace mira_gui::actions {
namespace {

using NamedGames = std::vector<std::pair<std::string, QString>>;

std::vector<std::string> Ids(const NamedGames& games) {
  std::vector<std::string> ids;
  for (const auto& [id, name] : games) ids.push_back(id);
  return ids;
}

std::map<std::string, QString> NamesById(const NamedGames& games) { return {games.begin(), games.end()}; }

// One notice naming every failed game, with the first failure's reason, hint and fix.
void NotifyFailures(QWidget* parent, const std::vector<GameFailure>& failures,
                    const std::map<std::string, QString>& names, const QString& verb) {
  if (failures.empty()) return;
  QStringList failed;
  for (const GameFailure& failure : failures) {
    const auto it = names.find(failure.id);
    failed << (it != names.end() ? it->second : QString::fromStdString(failure.id));
  }
  notify::FailedRequest(parent, QString("%1 %2.").arg(verb, failed.join(", ")), failures.front().error);
}

}  // namespace

void Launch(QWidget* parent, const std::string& id, std::function<void(bool tracked)> on_launched) {
  api::LaunchGameAsync(parent, id, [parent, id, on_launched](LaunchResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not launch the game.", result.error);
      return;
    }
    if (on_launched) on_launched(result.tracked);
  });
}

void Stop(QWidget* parent, const std::string& id) {
  api::StopGameAsync(parent, id, [parent, id](StopResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not stop the game.", result.error);
    }
  });
}

void Delete(QWidget* parent, const std::string& id, const QString& name,
            std::function<void()> on_deleted) {
  // Paths come from a detail fetch; the list summary carries neither
  // install_path nor data_dir. A failed fetch still offers the plain
  // remove, with both options disabled.
  api::GetGameAsync(parent, id, [parent, id, name, on_deleted](GameDetailResult detail) {
    const QString install_path =
        detail.ok ? QString::fromStdString(detail.game.install_path) : QString();
    const QString data_dir = detail.ok ? QString::fromStdString(detail.game.data_dir) : QString();
    const QString source = detail.ok ? QString::fromStdString(detail.game.source) : QString();

    const DeleteChoice choice = AskDeleteGame(parent, name, install_path, data_dir, source);
    if (!choice.confirmed) return;

    api::DeleteGameAsync(
        parent, id, choice.delete_files, choice.delete_prefix, choice.delete_metadata,
        [parent, name, on_deleted](DeleteResult result) {
          if (!result.ok) {
            notify::FailedRequest(parent, QString("Could not remove \"%1\".").arg(name), result.error);
            return;
          }
          if (on_deleted) on_deleted();
        });
  });
}

void RunInPrefix(QWidget* parent, const std::string& id, const std::string& install_path,
                 const QString& name) {
  RunInPrefixDialog dialog(id, install_path, name, parent);
  dialog.exec();
}

void FinishInstall(QWidget* parent, const std::string& id, std::function<void()> on_finished) {
  api::FinishInstallAsync(parent, id, [parent, id, on_finished](FinishInstallResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not mark this game as installed.", result.error);
      return;
    }
    if (on_finished) on_finished();
  });
}

void OpenInstallFolder(QWidget* parent, const std::string& install_path) {
  // install_path is already on GameSummary, so a fetch here would only ever
  // reproduce what the caller already has.
  if (install_path.empty()) {
    notify::Failed(parent, "Could not open the install folder.", "This game has no install path.");
    return;
  }
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(install_path)));
}

void ViewLog(QWidget* parent, const std::string& id, const QString& name) {
  LogViewerDialog dialog(id, name, parent);
  dialog.exec();
}

void RunWinetricks(QWidget* parent, const std::string& id, const QString& name) {
  api::GetGameAsync(parent, id, [parent, id, name](GameDetailResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not run winetricks.", result.error);
      return;
    }
    if (result.game.data_dir.empty()) {
      notify::FailedWithHint(parent, "Can't run winetricks yet.",
                             "This game has no Wine/Proton prefix provisioned.",
                             "Run something in its prefix first (\"Run in prefix…\"), which "
                             "provisions one on demand.");
      return;
    }
    WinetricksDialog dialog(id, name, parent);
    dialog.exec();
  });
}

void ToggleDesktopEntry(QWidget* parent, const std::string& id, bool currently_enabled) {
  const GameConfigEdit edit{"desktop_entries.enabled", "a boolean",
                            currently_enabled ? "false" : "true", false};
  api::PatchGameConfigAsync(
      parent, id, {edit}, [parent, currently_enabled](PatchGameConfigResult result) {
        if (!result.ok) {
          notify::FailedRequest(parent, "Could not update the desktop entry.", result.error);
          return;
        }
        // The only feedback there is: nothing in Mira's own window changes.
        notify::Notice(parent, currently_enabled ? "Removed from the application menu."
                                                 : "Added to the application menu.");
      });
}

void BatchDelete(QWidget* parent, const std::vector<std::pair<std::string, QString>>& games,
                 std::function<void()> on_done) {
  if (games.empty()) return;
  const DeleteChoice choice = AskDeleteGames(parent, static_cast<int>(games.size()));
  if (!choice.confirmed) return;

  // mirad itself never deletes a desktop-entry game's files or prefix.
  api::DeleteGamesAsync(
      parent, Ids(games), choice.delete_files, choice.delete_prefix, choice.delete_metadata,
      [parent, names = NamesById(games), on_done](DeleteGamesResult result) {
        if (!result.ok) {
          notify::FailedRequest(parent, "Could not remove the games.", result.error);
        } else if (!result.failed.empty()) {
          // Success needs no notice: the games leave the grid.
          NotifyFailures(parent, result.failed, names, "Could not remove");
        }
        if (on_done) on_done();
      });
}


void Relocate(QWidget* parent, const std::vector<std::pair<std::string, QString>>& games,
              std::function<void()> on_done) {
  if (games.empty()) return;
  const QString question =
      games.size() == 1
          ? QString("Move %1 into your games folder, and its prefix into the prefixes folder?")
                .arg(games.front().second)
          : QString("Move these %1 games into your games folder, and their prefixes into the "
                    "prefixes folder?")
                .arg(games.size());
  if (!notify::Confirm(parent, "Move to Mira's folders",
                       question + " Folders are named after the game. Games installed by a store "
                                  "(Steam, Epic, GOG, itch.io, Amazon) keep their install folder; only "
                                  "the prefix moves. Games from Lutris move completely and become Mira's "
                                  "own, since Lutris can't start them from there.",
                       "Move")) {
    return;
  }

  api::RelocateGamesAsync(
      parent, Ids(games), [parent, names = NamesById(games), on_done](RelocateLibraryResult result) {
        if (!result.ok) {
          notify::FailedRequest(parent, "Could not move the games.", result.error);
        } else {
          // Nothing on screen shows a path, so success gets a notice.
          if (result.moved > 0) {
            notify::Notice(parent, QString("Moved %1 game%2.").arg(result.moved).arg(result.moved == 1 ? "" : "s"));
          } else if (result.errors.empty()) {
            notify::Notice(parent, "Already in Mira's folders.");
          }
          NotifyFailures(parent, result.errors, names, "Could not move");
        }
        if (on_done) on_done();
      });
}

void BatchSetDesktopEntry(QWidget* parent, const std::vector<std::string>& ids, bool enabled) {
  if (ids.empty()) return;
  GamesPatch patch;
  patch.ids = ids;
  patch.config = {{"desktop_entries.enabled", "a boolean", enabled ? "true" : "false", false}};
  api::PatchGamesAsync(parent, patch, [parent, enabled](PatchGamesResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not update the application menu.", result.error);
      return;
    }
    notify::Notice(parent, enabled ? "Added to the application menu." : "Removed from the application menu.");
  });
}

}  // namespace mira_gui::actions
