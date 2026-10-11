#include "SourceRemoval.h"

#include <QCheckBox>
#include <QDir>
#include <QPushButton>
#include <QStringList>
#include <QWidget>
#include <algorithm>

#include "../app/Notify.h"
#include "../client/api/Stores.h"
#include "../system/PackageInstall.h"
#include "../widgets/PopupDialog.h"
#include "SourceText.h"

namespace mira_gui {

void RemoveSource(QWidget* parent, const SourceInfo& source, std::function<void()> on_removed) {
  const QString name = source.name;
  const std::string id = source.id.toStdString();
  api::GetRemovalPlanAsync(parent, id, [parent, id, name, on_removed](RemovalPlanResult plan) {
    if (!plan.ok) {
      notify::FailedRequest(parent, "Could not plan the removal.", plan.error);
      return;
    }
    const QString item = CopyFor(id).item;
    QStringList lines;
    const auto uninstalled =
        std::ranges::count_if(plan.games, [](const auto& g) { return !g.deletes.empty(); });
    if (uninstalled > 0)
      lines << QString("Uninstalls %1 %2%3:").arg(uninstalled).arg(item).arg(uninstalled == 1 ? "" : "s");
    for (const auto& game : plan.games) {
      if (!game.deletes.empty()) lines << "  • " + QString::fromStdString(game.name);
    }
    const auto dropped = static_cast<qsizetype>(plan.games.size()) - uninstalled;
    if (dropped > 0) {
      lines << QString("Removes %1 %2%3 from Mira only (their files stay where they are).")
                   .arg(dropped)
                   .arg(item)
                   .arg(dropped == 1 ? "" : "s");
    }
    if (!plan.program_dir.empty()) lines << "Deletes " + name + " itself.";
    if (!plan.uninstall.empty()) {
      lines << "Uninstalls " + name + " from your system (asks for your password).";
    } else if (!plan.packages.empty()) {
      lines << "Leaves " + system::Names(plan.packages) + " installed: remove it with your system's package manager.";
    }
    if (plan.signs_out) lines << "Signs you out of " + name + ".";
    if (!plan.kept.empty()) lines << (item == "app" ? "Keeps app data and settings (prefixes stay on disk)."
                                              : "Keeps game data and saves (prefixes stay on disk).");
    lines << name + " goes back to Add source, to set up again any time.";
    PopupDialog dialog(parent, notify::Level::Warning, "Remove " + name);
    dialog.SetMessage(lines.join("\n"));
    // Its settings and saves stay unless asked: they're the one thing here that can't be had again.
    QCheckBox* delete_data = nullptr;
    if (!plan.data.empty()) {
      QStringList folders;
      for (const std::string& path : plan.data) {
        const QString shown = QString::fromStdString(path);
        folders << "  • " + (shown.startsWith(QDir::homePath()) ? "~" + shown.mid(QDir::homePath().size()) : shown);
      }
      delete_data = new QCheckBox("Also delete " + name + "'s settings and saves", &dialog);
      dialog.AddWidget(delete_data);
      dialog.SetDetail("Your saves for its games are in these folders, and can't be recovered once deleted:\n" +
                       folders.join("\n"));
    }
    dialog.AddButton("Remove", /*accept=*/true);
    dialog.AddButton("Cancel", /*accept=*/false, /*default_button=*/true);
    if (dialog.exec() != QDialog::Accepted) return;
    const std::vector<std::string> uninstall = plan.uninstall;
    api::RemoveSourceAsync(parent, id, delete_data != nullptr && delete_data->isChecked(),
                           [parent, name, uninstall, on_removed](RemoveSourceResult r) {
      if (!r.ok) {
        notify::FailedRequest(parent, "Could not remove " + name + ".", r.error);
        return;
      }
      if (!r.problems.empty()) {
        QStringList problems;
        for (const std::string& problem : r.problems) problems << QString::fromStdString(problem);
        notify::Failed(parent, name + " was removed, but some steps failed.", problems.join("\n"));
      }
      on_removed();
      if (!uninstall.empty()) system::UninstallPackages(parent, uninstall, name, [](bool) {});
    });
  });
}

}  // namespace mira_gui
