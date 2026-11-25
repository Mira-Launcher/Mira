#include "GameActions.h"

#include <QCursor>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QMenu>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QWidget>
#include <map>
#include <memory>
#include <utility>

#include "../app/Notify.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../client/api/ModManagers.h"
#include "../components/Format.h"
#include "../components/WindowCards.h"
#include "../dialogs/LogWindow.h"
#include "../game/MoveGamesCard.h"
#include "../game/PrefixCards.h"
#include "../game/RemoveModManagerCard.h"
#include "../sources/Sources.h"
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
  notify::FailedRequest(parent, QString("%1 %2").arg(verb, failed.join(", ")),
                        failures.front().error);
}

// Launches from this app still waiting for mirad or a saves choice, by game.
std::map<std::string, int>& LaunchesUnderWay() {
  static std::map<std::string, int> launches;
  return launches;
}

}  // namespace

void Launch(QWidget* parent, const std::string& id, const std::string& option,
            std::function<void(bool tracked)> on_launched, std::function<void()> on_failed,
            const std::string& saves) {
  ++LaunchesUnderWay()[id];
  const auto done = [id] {
    if (--LaunchesUnderWay()[id] == 0) LaunchesUnderWay().erase(id);
  };
  // mirad holds the request while the game's saves are backed up or synced; the tile shows that
  // from game.saves_activity.
  api::LaunchGameAsync(
      parent, id, option, saves,
      [parent, id, option, on_launched, on_failed, saves, done](LaunchResult result) {
        auto* chooser = dynamic_cast<SavesChooser*>(parent->window());
        if (!result.ok && result.error.code == "saves_conflict" && saves.empty() &&
            chooser != nullptr) {
          chooser->ChooseSaves(id, [parent = QPointer<QWidget>(parent), id, option, on_launched,
                                    on_failed, done](const std::string& choice) {
            done();
            if (choice.empty() || parent.isNull()) {
              if (on_failed) on_failed();
              return;
            }
            Launch(parent, id, option, on_launched, on_failed, choice);
          });
          return;
        }
        done();
        if (!result.ok) {
          if (on_failed) on_failed();
          notify::FailedRequest(parent, "Could not launch the game", result.error);
          return;
        }
        if (!result.warning.empty())
          notify::Notice(parent, QString::fromStdString(result.warning), notify::Level::Warning);
        if (on_launched) on_launched(result.tracked);
      });
}

bool LaunchUnderWay(const std::string& id) {
  return LaunchesUnderWay().contains(id);
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
      notify::FailedRequest(parent, "Could not stop the game", result.error);
    }
  });
}

void RemoveGames(QWidget* parent, const std::vector<RemovalPlan>& plans, bool delete_prefix,
                 bool delete_saves, bool delete_outside) {
  // Each game asks only for what its plan says can go: files where they're Mira's or a store's
  // (or outside Mira's folders when asked), and a prefix no other game uses. Anything more is
  // refused, and would keep the game.
  std::map<std::pair<bool, bool>, std::vector<std::string>> groups;
  std::map<std::string, QString> names;
  for (const RemovalPlan& plan : plans) {
    const bool files = DeletesFiles(plan) || (delete_outside && plan.kind == "outside");
    groups[{files, delete_prefix && HasPrefixToDelete(plan)}].push_back(plan.id);
    names[plan.id] = QString::fromStdString(plan.name);
  }
  for (const auto& [flags, ids] : groups) {
    const auto [files, prefix] = flags;
    // Success needs no notice: the games leave the grid, a Steam game once Steam is done.
    api::DeleteGamesAsync(
        parent, ids, files, prefix, delete_saves, [parent, names](DeleteGamesResult result) {
          if (!result.ok) {
            notify::FailedRequest(parent, "Could not remove the games", result.error);
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

void OpenFolder(QWidget* parent, const std::string& folder) {
  if (folder.empty()) {
    notify::Failed(parent, "Could not open the folder", "There is no folder to open");
    return;
  }
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder)));
}

void ViewLog(QWidget* parent, const std::string& id, const QString& name) {
  LogWindow::Open(parent, "game:" + QString::fromStdString(id),
                  name.isEmpty() ? QString::fromStdString(id) : name);
}

void RunWinetricks(QWidget* parent, const std::string& id, const QString& name,
                   const QString& verb) {
  api::GetGameAsync(parent, id, [parent, id, name, verb](GameDetailResult result) {
    if (!result.ok) {
      notify::FailedRequest(parent, "Could not run winetricks", result.error);
      return;
    }
    if (result.game.data_dir.empty()) {
      notify::FailedWithHint(parent, "Can't run winetricks yet",
                             "This game has no Wine/Proton prefix provisioned",
                             "Run something in its prefix first (\"Run in prefix…\"), which "
                             "provisions one on demand.");
      return;
    }
    cards::Show(parent, new WinetricksCard(id, name, verb));
  });
}

void ToggleDesktopEntry(QWidget* parent, const std::string& id, bool currently_enabled) {
  const GameConfigEdit edit{"desktop_entries.enabled", "a boolean",
                            currently_enabled ? "false" : "true", false};
  // Success needs no notice: the menu entry itself appears or goes.
  api::PatchGameConfigAsync(parent, id, {edit}, [parent](PatchGameConfigResult result) {
    if (!result.ok)
      notify::FailedRequest(parent, "Could not update the desktop entry", result.error);
  });
}

namespace {

// Moves them, saying how it went.
void MoveGames(QWidget* parent, const std::vector<std::string>& ids,
               const std::map<std::string, QString>& names, std::function<void()> on_done);

}  // namespace

void RemoveModManager(QWidget* parent, const std::string& id, const QString& name,
                      const std::string& manager) {
  const SourceInfo* info = FindSourceInfo(QString::fromStdString(manager));
  if (info == nullptr) return;
  // Shown at once; its rows fill in when mirad has looked through the prefix.
  auto* card = new RemoveModManagerCard(name, *info);
  QObject::connect(card, &RemoveModManagerCard::Confirmed, parent,
                   [parent, card, id, name, manager, manager_name = info->name] {
                     cards::Close(card);
                     api::RemoveModManagerFromGameAsync(
                         parent, manager, id, [parent, name, manager_name](StoreActionResult r) {
                           if (!r.ok)
                             notify::FailedRequest(
                                 parent, "Could not remove " + manager_name + " from " + name + ".",
                                 r.error);
                         });
                   });
  cards::Show(parent, card);
  api::GetModManagerPurgeAsync(
      card, manager, id, [card](ModManagerPurgeResult summary) { card->SetSummary(summary); });
}

void Relocate(QWidget* parent, const std::vector<std::pair<std::string, QString>>& games,
              std::function<void()> on_done) {
  if (games.empty()) return;
  api::GetRelocationPlansAsync(
      parent, Ids(games), [parent, games, on_done](RelocationPlansResult result) {
        if (!result.ok) {
          notify::FailedRequest(parent, "Could not move the games", result.error);
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
      notify::FailedRequest(parent, "Could not move the games", result.error);
    } else {
      // Nothing on screen shows a path, so success gets a notice.
      if (result.moved > 0) {
        notify::Notice(parent, "Moved " + Counted(result.moved, "game"));
      } else if (result.skipped > 0) {
        notify::Notice(parent,
                       result.skipped == 1
                           ? QString("That game was removed meanwhile")
                           : QString("Those %1 games were removed meanwhile").arg(result.skipped));
      } else if (result.errors.empty()) {
        notify::Notice(parent, "Already in Mira's folders");
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
      notify::FailedRequest(parent, "Could not update the application menu", result.error);
  });
}

}  // namespace mira_gui::actions
