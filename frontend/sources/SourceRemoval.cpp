#include "SourceRemoval.h"

#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QStringList>
#include <QWidget>
#include <algorithm>

#include "../app/Notify.h"
#include "../client/api/Stores.h"
#include "../components/Button.h"
#include "../components/Format.h"
#include "../components/PopupDialog.h"
#include "../system/PackageInstall.h"
#include "SourceText.h"
#include "Sources.h"

namespace mira_gui {

void RemoveSource(QWidget* parent, const SourceInfo& source, std::function<void()> on_removed) {
  const QString name = source.name;
  const std::string id = source.id.toStdString();
  api::GetRemovalPlanAsync(parent, id, [parent, id, name, on_removed](RemovalPlanResult plan) {
    if (!plan.ok) {
      notify::FailedRequest(parent, "Could not plan the removal", plan.error);
      return;
    }
    const QString item = CopyFor(id).item;
    QStringList lines;
    const auto uninstalled =
        std::ranges::count_if(plan.games, [](const auto& g) { return !g.deletes.empty(); });
    if (uninstalled > 0) lines << "Uninstalls " + Counted(uninstalled, item) + ":";
    for (const auto& game : plan.games) {
      if (!game.deletes.empty()) lines << "  • " + QString::fromStdString(game.name);
    }
    // An emulator's games in Mira's folders are asked about below.
    const auto dropped = static_cast<qsizetype>(plan.games.size()) - uninstalled - plan.roms;
    if (dropped > 0) {
      lines << "Removes " + Counted(dropped, item) +
                   " from Mira only (their files stay where they are).";
    }
    if (!plan.purged.empty()) {
      lines << "Takes the mods it deployed out of these games, puts back the game files it set "
               "aside, and deletes its mods, downloads and settings for them:";
      for (const std::string& game : plan.purged) lines << "  • " + QString::fromStdString(game);
    }
    if (!plan.program_dir.empty()) lines << "Deletes " + name + " itself.";
    if (!plan.uninstall.empty()) {
      lines << "Uninstalls " + name + " from your system (asks for your password).";
    } else if (!plan.packages.empty()) {
      lines << "Leaves " + system::Names(plan.packages) +
                   " installed: remove it with your system's package manager.";
    }
    if (plan.signs_out) lines << "Signs you out of " + name + ".";
    if (!plan.tools.empty()) {
      QStringList tools;
      for (const std::string& tool : plan.tools) tools << QString::fromStdString(tool);
      lines << "Deletes " + tools.join(" and ") + ", which Mira downloaded.";
    }
    if (!plan.kept.empty())
      lines << (item == "app" ? "Keeps app data and settings (prefixes stay on disk)"
                              : "Keeps game data and saves (prefixes stay on disk)");
    lines << name + " goes back to Add source, to set up again any time.";
    PopupDialog dialog(parent, Tone::Warning, "Remove " + name);
    dialog.SetMessage(lines.join("\n"));
    // Its settings and saves stay unless asked: they're the one thing here that can't be had again.
    QCheckBox* delete_data = nullptr;
    if (!plan.data.empty()) {
      QStringList folders;
      for (const std::string& path : plan.data) {
        folders << "  • " + HomeRelative(QString::fromStdString(path));
      }
      delete_data = new QCheckBox("Also delete " + name + "'s settings and saves", &dialog);
      dialog.AddWidget(delete_data);
      dialog.SetDetail(
          "Your saves for its games are in these folders, and can't be recovered once deleted:\n" +
          folders.join("\n"));
    }
    // Its games in Mira's folders: kept, deleted, or moved to another emulator that plays them.
    QComboBox* roms = nullptr;
    if (plan.roms > 0) {
      roms = new QComboBox(&dialog);
      roms->setAccessibleName("Its games");
      roms->addItem("Keep " + Counted(plan.roms, item) + " in Mira's folder", QString());
      roms->addItem("Delete " + Counted(plan.roms, item), QString("delete"));
      for (const std::string& to : plan.move_to) {
        const SourceInfo* emulator = FindSourceInfo(QString::fromStdString(to));
        roms->addItem("Move " + Counted(plan.roms, item) + " to " +
                          (emulator ? emulator->name : QString::fromStdString(to)),
                      QString::fromStdString(to));
      }
      dialog.AddWidget(roms);
    }
    dialog.AddButton("Remove", /*accept=*/true)->SetKind(ButtonKind::Danger);
    dialog.AddButton("Cancel", /*accept=*/false, /*default_button=*/true);
    if (dialog.exec() != QDialog::Accepted) return;
    const std::vector<std::string> uninstall = plan.uninstall;
    const QString rom_choice = roms != nullptr ? roms->currentData().toString() : QString();
    api::RemoveSourceAsync(
        parent, id, delete_data != nullptr && delete_data->isChecked(), rom_choice == "delete",
        rom_choice == "delete" ? std::string() : rom_choice.toStdString(),
        [parent, name, uninstall, on_removed](RemoveSourceResult r) {
          if (!r.ok) {
            notify::FailedRequest(parent, "Could not remove " + name + ".", r.error);
            return;
          }
          if (!r.problems.empty()) {
            QStringList problems;
            for (const std::string& problem : r.problems)
              problems << QString::fromStdString(problem);
            notify::Failed(parent, name + " was removed, but some steps failed.",
                           problems.join("\n"));
          }
          // Over the window: on_removed can close the page.
          if (!uninstall.empty())
            system::UninstallPackages(parent->window(), uninstall, name, [](bool) {});
          on_removed();
        });
  });
}

}  // namespace mira_gui
