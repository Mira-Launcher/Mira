#include "GameMenus.h"

#include <QAction>
#include <QDesktopServices>
#include <QLineEdit>
#include <QMenu>
#include <QWidget>
#include <QWidgetAction>
#include <algorithm>
#include <memory>

#include "../app/Notify.h"
#include "../app/Shortcuts.h"
#include "../client/api/Artwork.h"
#include "../client/api/Emulators.h"
#include "../client/api/Games.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "GameActions.h"
#include "GameLibraryModel.h"
#include "GamePresentation.h"
#include "PlayChoices.h"

namespace mira_gui {

GameMenus::GameMenus(QWidget* parent, Host host)
    : QObject(parent), parent_(parent), host_(std::move(host)) {}

void GameMenus::ShowGameMenu(const std::string& id, const QPoint& global_pos,
                             const std::function<void(QMenu&)>& extra) {
  // Copied, not kept as a pointer: an event landing while the menu is open can move the library's
  // rows.
  const GameSummary* found = host_.find(id);
  if (found == nullptr) return;
  const GameSummary game = *found;
  const QString name = QString::fromStdString(game.name);
  const std::string& status = game.status;
  const bool running = game.running;

  QMenu menu(parent_);
  QAction* play = menu.addAction(running ? "Stop" : RunVerb(game));
  play->setEnabled(CanPlayOrStop(game));
  if (host_.install_actions && host_.install_actions(menu, id)) menu.addSeparator();
  // The game's other ways to start, and its tools.
  if (!running && CanPlayOrStop(game)) {
    if (const std::vector<PlayChoice> choices = PlayChoices(game, /*all=*/true);
        choices.size() > 1) {
      QMenu* with = menu.addMenu("Play with");
      for (const PlayChoice& choice : choices) {
        with->addAction(choice.label, this,
                        [this, id, option = choice.option] { host_.launch(id, option); });
      }
    }
  }
  for (const PlayChoice& tool : ToolChoices(game)) {
    menu.addAction("Open " + tool.label, this,
                   [this, id, option = tool.option] { actions::Launch(parent_, id, option, {}); });
  }
  // Stopping a game that isn't running closes the tools it opened.
  if (!running && !game.tools.empty()) {
    const QString names = QString::fromStdString(ToolNames(game));
    menu.addAction("Close " + names, this, [this, id, names] {
      api::StopGameAsync(parent_, id, [this, names](StopResult result) {
        if (!result.ok)
          notify::FailedRequest(parent_, "Could not close " + names + ".", result.error);
      });
    });
  }
  if (extra) extra(menu);
  // An emulator's game moves to another one for its console; filled in when mirad answers.
  const SourceInfo* source = FindSourceInfo(QString::fromStdString(game.source));
  if (source != nullptr && source->kind == SourceInfo::Kind::Emulator) {
    QMenu* run_with = menu.addMenu("Move to emulator");
    run_with->setEnabled(false);
    api::GetGameEmulatorsAsync(
        run_with, id, [this, run_with, id, name](EmulatorChoicesResult result) {
          // Say why there's nothing to pick rather than leave the entry greyed out.
          if (!result.ok)
            run_with->addAction("Couldn't list the emulators")->setEnabled(false);
          else if (result.emulators.empty())
            run_with->addAction("No other installed emulator plays this")->setEnabled(false);
          for (const EmulatorChoice& choice : result.emulators) {
            const QString emulator = QString::fromStdString(choice.name);
            run_with->addAction(emulator, this, [this, id, name, emulator, to = choice.id] {
              api::MoveGameToEmulatorAsync(
                  parent_, id, to, [this, name, emulator](StoreActionResult moved) {
                    if (!moved.ok)
                      notify::FailedRequest(
                          parent_, "Could not move " + name + " to " + emulator + ".", moved.error);
                  });
            });
          }
          run_with->setEnabled(true);
        });
  }
  QAction* details = menu.addAction("Game settings…");
  QAction* folder = menu.addAction("Open install folder");
  if (!game.store_url.empty()) {
    const QUrl page(QString::fromStdString(game.store_url));
    connect(menu.addAction("Open store page"), &QAction::triggered, parent_,
            [page] { QDesktopServices::openUrl(page); });
  }
  QAction* game_details = menu.addAction("Game details…");
  const bool pinned = IsPinned(game);
  QAction* toggle_pinned = menu.addAction(pinned ? "Unpin" : "Pin to sidebar");
  menu.addSeparator();
  // Both halves of the needs_install escape hatch: run the installer inside
  // this game's prefix, then say it worked. "Run in prefix" is offered for
  // every game; only needs_install can be "marked installed".
  const bool installing = !host_.install_text(id).isEmpty();
  QAction* install = menu.addAction(installing ? "Installing…" : "Install…");
  install->setEnabled((status == "needs_install" || status == "broken") && !installing);
  install->setToolTip("Run this game's installer, or pick a different one");
  QAction* run_in_prefix =
      menu.addAction(game.platform == "native" ? "Run a program…" : "Run in prefix…");
  QAction* finish_install = menu.addAction("Mark as installed");
  finish_install->setEnabled(status == "needs_install");
  finish_install->setToolTip(status == "needs_install"
                                 ? "Mark this game as ready once its executable points at the "
                                   "installed program"
                                 : "Only available for a game that still needs installing");
  QAction* refresh_metadata = menu.addAction("Refresh metadata && cover art");
  QAction* view_log = menu.addAction(running ? "View live log…" : "View log…");
  QAction* winetricks = menu.addAction("Winetricks…");
  QAction* relocate = menu.addAction("Move to Mira's folders…");
  relocate->setToolTip("Move this game's files and prefix into the library and prefix folders");
  // An emulator's game has no installer, prefix or folder of Mira's to move it to.
  const bool emulated = source != nullptr && source->kind == SourceInfo::Kind::Emulator;
  // Nor store info for Game details to show.
  for (QAction* action :
       {install, run_in_prefix, finish_install, winetricks, relocate, game_details})
    action->setVisible(!emulated);
  const bool native = game.platform == "native";
  winetricks->setEnabled(!native);
  winetricks->setToolTip(native ? "Native games have no Wine or Proton prefix." : QString());
  // The resolved setting isn't on GameSummary. The menu opens at once and the
  // entry fills in when mirad answers, a moment later.
  QAction* desktop_entry = menu.addAction("Desktop entry");
  desktop_entry->setEnabled(false);
  auto desktop_entry_enabled = std::make_shared<bool>(true);
  api::GetGameConfigAsync(
      &menu, id, [desktop_entry, desktop_entry_enabled](GameConfigResult result) {
        if (!result.ok) {
          desktop_entry->setText("Desktop entry (couldn't read the setting)");
          return;
        }
        const auto entry = std::ranges::find(result.entries, std::string("desktop_entries.enabled"),
                                             &GameConfigEntry::key);
        if (entry == result.entries.end()) return;
        *desktop_entry_enabled = entry->value_display == "true";
        desktop_entry->setText(*desktop_entry_enabled ? "Remove desktop entry"
                                                      : "Add desktop entry");
        desktop_entry->setEnabled(true);
      });
  menu.addSeparator();
  const bool hidden = IsHidden(game);
  QAction* toggle_hidden = menu.addAction(hidden ? "Unhide" : "Hide");
  toggle_hidden->setToolTip(hidden ? "Show this game in the library again"
                                   : "Keep this game out of the library until you ask for it " +
                                         shortcuts::HiddenGamesHint());
  const bool app = IsApp(game);
  QAction* toggle_app = menu.addAction(app ? "Mark as game" : "Mark as app");
  toggle_app->setToolTip(
      "An app is a program rather than a game: no playtime, and kept out of Continue");
  AddTagsMenu(menu, {id});
  menu.addSeparator();
  // A store's games are uninstalled; the card says what any other game loses.
  const bool store = game.source == "steam" || game.source == "office" ||
                     (source != nullptr && source->kind == SourceInfo::Kind::Store);
  QAction* remove = menu.addAction(store ? "Uninstall…" : "Remove…");

  QAction* chosen = menu.exec(global_pos);
  if (chosen == play) {
    running ? host_.toggle_running(id)
            : host_.launch(id, {});  // the default; other choices are under Play with
  } else if (chosen == details) {
    host_.open_settings(id);
  } else if (chosen == folder) {
    actions::OpenInstallFolder(parent_, game.install_path);
  } else if (chosen == game_details) {
    host_.open_details(id);
  } else if (chosen == install) {
    host_.offer_install(id);
  } else if (chosen == relocate) {
    actions::Relocate(parent_, {{id, name}}, nullptr);
  } else if (chosen == run_in_prefix) {
    actions::RunInPrefix(parent_, id, game.install_path, name);
  } else if (chosen == finish_install) {
    actions::FinishInstall(parent_, id, nullptr);
  } else if (chosen == refresh_metadata) {
    host_.refresh_metadata(id);
  } else if (chosen == view_log) {
    actions::ViewLog(parent_, id, name);
  } else if (chosen == winetricks) {
    actions::RunWinetricks(parent_, id, name);
  } else if (chosen == desktop_entry) {
    actions::ToggleDesktopEntry(parent_, id, *desktop_entry_enabled);
  } else if (chosen == toggle_pinned) {
    ToggleTag(id, tags::kPinned);
  } else if (chosen == toggle_hidden) {
    ToggleTag(id, tags::kHidden);
  } else if (chosen == toggle_app) {
    ToggleTag(id, tags::kApp);
  } else if (chosen == remove) {
    host_.remove({id});
  }
}

void GameMenus::ShowBatchMenu(const std::vector<std::string>& ids, const QPoint& global_pos) {
  // Named before the menu opens: an event landing while it's up can replace games_.
  std::vector<std::pair<std::string, QString>> named;
  named.reserve(ids.size());
  for (const std::string& id : ids) {
    const GameSummary* game = host_.find(id);
    named.emplace_back(
        id, game != nullptr ? QString::fromStdString(game->name) : QString::fromStdString(id));
  }
  const int count = static_cast<int>(ids.size());

  int pinned = 0;
  int hidden = 0;
  int apps = 0;
  for (const std::string& id : ids) {
    const GameSummary* game = host_.find(id);
    if (game != nullptr && IsPinned(*game)) ++pinned;
    if (game != nullptr && IsHidden(*game)) ++hidden;
    if (game != nullptr && IsApp(*game)) ++apps;
  }

  // Each offered for the games it would change, so a mixed selection gets both.
  QMenu menu(parent_);
  QAction* refresh_metadata =
      menu.addAction(QString("Refresh metadata && cover art (%1)").arg(count));
  QAction* pin =
      pinned < count ? menu.addAction(QString("Pin to sidebar (%1)").arg(count - pinned)) : nullptr;
  QAction* unpin = pinned > 0 ? menu.addAction(QString("Unpin (%1)").arg(pinned)) : nullptr;
  QAction* hide =
      hidden < count ? menu.addAction(QString("Hide (%1)").arg(count - hidden)) : nullptr;
  if (hide != nullptr) {
    hide->setToolTip("Keep these games out of the library until you ask for them " +
                     shortcuts::HiddenGamesHint());
  }
  QAction* unhide = hidden > 0 ? menu.addAction(QString("Unhide (%1)").arg(hidden)) : nullptr;
  QAction* mark_app =
      apps < count ? menu.addAction(QString("Mark as app (%1)").arg(count - apps)) : nullptr;
  QAction* mark_game = apps > 0 ? menu.addAction(QString("Mark as game (%1)").arg(apps)) : nullptr;
  auto* desktop_menu = menu.addMenu("Desktop entry");
  QAction* add_desktop_entry = desktop_menu->addAction("Add to application menu");
  QAction* remove_desktop_entry = desktop_menu->addAction("Remove from application menu");
  QAction* relocate = menu.addAction(QString("Move to Mira's folders… (%1)").arg(count));
  AddTagsMenu(menu, ids);
  menu.addSeparator();
  QAction* remove = menu.addAction(QString("Remove… (%1)").arg(count));

  QAction* chosen = menu.exec(global_pos);
  if (chosen == nullptr) return;  // dismissed; also keeps it from matching an action left out above
  if (chosen == refresh_metadata) {
    // Activity shows it going; a notice sums it up at the end.
    api::RefreshMetadataManyAsync(this, ids, [this](MetadataBatchResult result) {
      if (!result.ok) {
        notify::FailedRequest(parent_, "Could not refresh metadata.", result.error);
      } else {
        notify::Notice(parent_, BatchRefreshSummary(result));
      }
    });
  } else if (chosen == pin || chosen == unpin) {
    SetTag(ids, tags::kPinned, chosen == pin);
  } else if (chosen == hide || chosen == unhide) {
    SetTag(ids, tags::kHidden, chosen == hide);
  } else if (chosen == mark_app || chosen == mark_game) {
    SetTag(ids, tags::kApp, chosen == mark_app);
  } else if (chosen == add_desktop_entry) {
    actions::BatchSetDesktopEntry(parent_, ids, /*enabled=*/true);
  } else if (chosen == remove_desktop_entry) {
    actions::BatchSetDesktopEntry(parent_, ids, /*enabled=*/false);
  } else if (chosen == relocate) {
    actions::Relocate(parent_, named, nullptr);
  } else if (chosen == remove) {
    host_.remove(ids);
  }
}

void GameMenus::SetTag(const std::vector<std::string>& ids, const std::string& tag, bool present) {
  // Matched ignoring case, like mirad's folder tags, so "rpg" isn't added beside "RPG".
  GamesPatch patch;
  for (const std::string& id : ids) {
    const GameSummary* game = host_.find(id);
    if (game == nullptr) continue;
    const auto has =
        std::ranges::find_if(game->tags, [&](const std::string& t) { return SameTag(t, tag); });
    if ((has != game->tags.end()) == present) continue;
    patch.ids.push_back(id);
    if (!present && std::ranges::find(patch.remove_tags, *has) == patch.remove_tags.end()) {
      patch.remove_tags.push_back(*has);  // every spelling the games use
    }
  }
  if (patch.ids.empty()) return;
  if (present) patch.add_tags.push_back(tag);

  const bool one = patch.ids.size() == 1;
  api::PatchGamesAsync(this, patch, [this, tag, one](PatchGamesResult result) {
    if (!result.ok) {
      const QString games = one ? "this game's" : "these games'";
      notify::FailedRequest(
          parent_,
          tag == tags::kHidden ? QString("Could not change %1 visibility.").arg(games)
          : tag == tags::kApp
              ? QString("Could not change what %1 marked as.").arg(one ? "this is" : "these are")
          : tag == tags::kPinned ? QString("Could not change whether %1 pinned.")
                                       .arg(one ? "this game is" : "these games are")
                                 : QString("Could not change %1 tags.").arg(games),
          result.error);
      return;
    }
    // Applied from the reply rather than waiting for games.updated, so the
    // change feels instant. No toast: the games visibly moving is the feedback.
    host_.upsert(result.games);
  });
}

void GameMenus::AddTagsMenu(QMenu& menu, const std::vector<std::string>& ids) {
  const auto known = [](const std::vector<std::string>& list, const std::string& tag) {
    return std::ranges::any_of(list, [&](const std::string& t) { return SameTag(t, tag); });
  };
  // Every tag in the library in the Tags page's order, the folder tags of the games' library
  // folders under their own heading.
  std::vector<std::string> folder_tags;
  for (const std::string& id : ids) {
    const GameSummary* game = host_.find(id);
    if (game == nullptr) continue;
    for (const std::string& tag : game->folder_tags.value_or(std::vector<std::string>())) {
      if (!known(folder_tags, tag)) folder_tags.push_back(tag);
    }
  }
  std::vector<std::string> others;
  for (const std::string& tag : TagOrder(host_.library())) {
    if (!known(folder_tags, tag)) others.push_back(tag);
  }
  std::vector<std::string> tags = folder_tags;
  tags.insert(tags.end(), others.begin(), others.end());

  QMenu* submenu = menu.addMenu("Tags");
  submenu->setToolTipsVisible(true);
  const int count = static_cast<int>(ids.size());
  // Under headings (disabled rows), not icons on the tags: Fusion draws a checked action's icon in
  // its tick's place, and a QMenu section's text not at all.
  const auto heading = [submenu](const QIcon& icon, const QString& text) {
    submenu->addAction(icon, text)->setEnabled(false);
  };
  if (!folder_tags.empty()) heading(icons::For(icons::Glyph::Folder), "Folders");
  for (const std::string& tag : tags) {
    if (!folder_tags.empty() && !others.empty() && tag == others.front()) {
      submenu->addSeparator();
      heading(QIcon(), "Other tags");
    }
    const auto having = std::ranges::count_if(ids, [&](const std::string& id) {
      const GameSummary* game = host_.find(id);
      return game != nullptr &&
             std::ranges::any_of(game->tags, [&](const std::string& t) { return SameTag(t, tag); });
    });
    const QString name = QString::fromStdString(tag);
    QAction* action = submenu->addAction(
        having > 0 && having < count ? QString("%1 (%2 of %3)").arg(name).arg(having).arg(count)
                                     : name);
    action->setCheckable(true);
    action->setChecked(having == count);
    // A partly tagged selection gets the tag on the rest; a fully tagged one loses it.
    connect(action, &QAction::triggered, this,
            [this, ids, tag, all = having == count] { SetTag(ids, tag, !all); });
  }
  if (!tags.empty()) submenu->addSeparator();
  auto* field = new QLineEdit(submenu);
  field->setObjectName("menu_field");
  field->setPlaceholderText(count == 1 ? "New tag" : QString("New tag for all %1").arg(count));
  auto* field_action = new QWidgetAction(submenu);
  field_action->setDefaultWidget(field);
  submenu->addAction(field_action);
  connect(field, &QLineEdit::returnPressed, this, [this, ids, field, &menu] {
    for (const QString& part : field->text().split(',', Qt::SkipEmptyParts)) {
      const std::string tag = part.trimmed().toStdString();
      if (!tag.empty()) SetTag(ids, tag, /*present=*/true);
    }
    menu.close();
  });
}

void GameMenus::ToggleTag(const std::string& id, const std::string& tag) {
  const GameSummary* game = host_.find(id);
  if (game != nullptr) SetTag({id}, tag, !HasTag(*game, tag));
}

}  // namespace mira_gui
