#include "GameActions.h"

#include <QCursor>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QMenu>
#include <QStringList>
#include <QUrl>
#include <QWidget>
#include <map>
#include <memory>
#include <utility>

#include "../app/Notify.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../dialogs/LogWindow.h"
#include "../game/MoveGamesCard.h"
#include "../game/PrefixCards.h"
#include "../widgets/WindowCards.h"
#include "PlayChoices.h"

namespace mira_gui::actions {
namespace {

using NamedGames = std::vector<std::pair<std::string, QString>>;

std::vector<std::string> Ids(const NamedGames& games) {
  std::vector<std::string> ids;
  for (const auto& [id, name] : games) ids.push_back(id);
  return ids;
}

std::map<std::string, QString> NamesById(const NamedGames& games) {
  return {games.begin(), games.end()};
}

// One notice naming every failed game, with the first failure's reason, hint and fix.
void NotifyFailures(QWidget* parent, const std::vector<GameFailure>& failures,
                    const std::map<std::string, QString>& names, const QString& verb) {
  if (failures.empty()) return;
  QStringList failed;
  for (const GameFailure& failure : failures) {
    const auto it = names.find(failure.id);
    failed << (it != names.end() ? it->second : QString::fromStdString(failure.id));
  }
  notify::FailedRequest(parent, QString("%1 %2.").arg(verb, failed.join(", ")),
                        failures.front().error);
}

}  // namespace

void Launch(QWidget* parent, const std::string& id, const std::string& option,
            std::function<void(bool tracked)> on_launched, std::function<void()> on_failed) {
  // mirad holds the request while the game's saves are backed up; the tile shows that from
  // game.saves_activity.
  api::LaunchGameAsync(
      parent, id, option, [parent, id, on_launched, on_failed](LaunchResult result) {
        if (!result.ok) {
          if (on_failed) on_failed();
          notify::FailedRequest(parent, "Could not launch the game.", result.error);
          return;
        }
        if (on_launched) on_launched(result.tracked);
      });
}

void ChoosePlay(QWidget* parent, const GameSummary& game,
                const std::function<void(const std::string& option)>& launch) {
  const bool all = QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
  const std::vector<PlayChoice> choices = PlayChoices(game, all);
  const std::vector<PlayChoice> tools = all ? ToolChoices(game) : std::vector<PlayChoice>();
  if (choices.size() == 1 && tools.empty()) return launch(choices.front().option);
  QMenu menu(parent);
  for (const PlayChoice& choice : choices) {
    menu.addAction(choice.label, [&launch, option = choice.option] { launch(option); });
  }
  if (!tools.empty()) menu.addSection("Tools");
  for (const PlayChoice& tool : tools) {
    menu.addAction(tool.label, [parent, id = game.id, option = tool.option] {
      Launch(parent, id, option, {});
    });
  }
  menu.exec(QCursor::pos());
}

void Stop(QWidget* parent, const std::string& id) {
  api::StopGameAsync(parent, id, [parent, id](StopResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not stop the game.", result.error);
    }
  });
}

void RemoveGames(QWidget* parent, const std::vector<RemovalPlan>& plans, bool delete_prefix,
                 bool delete_saves) {
  // Files only where mirad's plan says they're Mira's or a store's: an explicit delete_files on
  // another app's files is refused, and would keep the game.
  std::vector<std::string> deleting, forgetting;
  std::map<std::string, QString> names;
  for (const RemovalPlan& plan : plans) {
    (DeletesFiles(plan) ? deleting : forgetting).push_back(plan.id);
    names[plan.id] = QString::fromStdString(plan.name);
  }
  for (const auto& [ids, files] : {std::pair{deleting, true}, std::pair{forgetting, false}}) {
    if (ids.empty()) continue;
    // Success needs no notice: the games leave the grid, a Steam game once Steam is done.
    api::DeleteGamesAsync(
        parent, ids, files, delete_prefix, delete_saves, [parent, names](DeleteGamesResult result) {
          if (!result.ok) {
            notify::FailedRequest(parent, "Could not remove the games.", result.error);
          } else if (!result.failed.empty()) {
            NotifyFailures(parent, result.failed, names, "Could not remove");
          }
        });
  }
}

void RunInPrefix(QWidget* parent, const std::string& id, const std::string& install_path,
                 const QString& name) {
  cards::Show(parent, new RunInPrefixCard(id, install_path, name));
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
  LogWindow::Open(parent, "game:" + QString::fromStdString(id),
                  name.isEmpty() ? QString::fromStdString(id) : name);
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
    cards::Show(parent, new WinetricksCard(id, name));
  });
}

void ToggleDesktopEntry(QWidget* parent, const std::string& id, bool currently_enabled) {
  const GameConfigEdit edit{"desktop_entries.enabled", "a boolean",
                            currently_enabled ? "false" : "true", false};
  // Success needs no notice: the menu entry itself appears or goes.
  api::PatchGameConfigAsync(parent, id, {edit}, [parent](PatchGameConfigResult result) {
    if (!result.ok)
      notify::FailedRequest(parent, "Could not update the desktop entry.", result.error);
  });
}

namespace {

// Moves them, saying how it went.
void MoveGames(QWidget* parent, const std::vector<std::string>& ids,
               const std::map<std::string, QString>& names, std::function<void()> on_done);

}  // namespace

void Relocate(QWidget* parent, const std::vector<std::pair<std::string, QString>>& games,
              std::function<void()> on_done) {
  if (games.empty()) return;
  api::GetRelocationPlansAsync(
      parent, Ids(games), [parent, games, on_done](RelocationPlansResult result) {
        if (!result.ok) {
          notify::FailedRequest(parent, "Could not move the games.", result.error);
          return;
        }
        if (result.plans.empty()) return;  // removed while asking
        std::vector<std::string> measured;
        for (const RelocationPlan& plan : result.plans) {
          if (MoveGamesCard::Moves(plan)) measured.push_back(plan.id);
        }
        auto* card = new MoveGamesCard(std::move(result.plans));
        QObject::connect(card, &MoveGamesCard::Confirmed, parent,
                         [parent, card, games, on_done](const std::vector<std::string>& ids) {
                           cards::Close(card);
                           MoveGames(parent, ids, NamesById(games), on_done);
                         });
        cards::Show(parent, card);
        if (measured.empty()) return;
        api::GetRemovalSizesAsync(card, measured, [card](RemovalSizesResult sizes) {
          if (sizes.ok) card->SetSizes(sizes.sizes);
        });
      });
}

namespace {

void MoveGames(QWidget* parent, const std::vector<std::string>& ids,
               const std::map<std::string, QString>& names, std::function<void()> on_done) {
  api::RelocateGamesAsync(parent, ids, [parent, names, on_done](RelocateLibraryResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not move the games.", result.error);
    } else {
      // Nothing on screen shows a path, so success gets a notice.
      if (result.moved > 0) {
        notify::Notice(
            parent,
            QString("Moved %1 game%2.").arg(result.moved).arg(result.moved == 1 ? "" : "s"));
      } else if (result.skipped > 0) {
        notify::Notice(parent,
                       result.skipped == 1
                           ? QString("That game was removed meanwhile.")
                           : QString("Those %1 games were removed meanwhile.").arg(result.skipped));
      } else if (result.errors.empty()) {
        notify::Notice(parent, "Already in Mira's folders.");
      }
      NotifyFailures(parent, result.errors, names, "Could not move");
    }
    if (on_done) on_done();
  });
}

}  // namespace

void BatchSetDesktopEntry(QWidget* parent, const std::vector<std::string>& ids, bool enabled) {
  if (ids.empty()) return;
  GamesPatch patch;
  patch.ids = ids;
  patch.config = {{"desktop_entries.enabled", "a boolean", enabled ? "true" : "false", false}};
  api::PatchGamesAsync(parent, patch, [parent](PatchGamesResult result) {
    if (!result.ok)
      notify::FailedRequest(parent, "Could not update the application menu.", result.error);
  });
}

}  // namespace mira_gui::actions
