#include "SourceRemoval.h"

#include <QStringList>
#include <QWidget>
#include <algorithm>

#include "../app/Notify.h"
#include "../client/api/Stores.h"

namespace mira_gui {

void RemoveSource(QWidget* parent, const SourceInfo& source, std::function<void()> on_removed) {
  const QString name = source.name;
  const std::string id = source.id.toStdString();
  api::GetRemovalPlanAsync(parent, id, [parent, id, name, on_removed](RemovalPlanResult plan) {
    if (!plan.ok) {
      notify::FailedRequest(parent, "Could not plan the removal.", plan.error);
      return;
    }
    QStringList lines;
    const auto uninstalled =
        std::ranges::count_if(plan.games, [](const auto& g) { return !g.deletes.empty(); });
    if (uninstalled > 0)
      lines << QString("Uninstalls %1 game%2:").arg(uninstalled).arg(uninstalled == 1 ? "" : "s");
    for (const auto& game : plan.games) {
      if (!game.deletes.empty()) lines << "  • " + QString::fromStdString(game.name);
    }
    const auto dropped = static_cast<qsizetype>(plan.games.size()) - uninstalled;
    if (dropped > 0) {
      lines << QString("Removes %1 game%2 from Mira only (their files stay where they are).")
                   .arg(dropped)
                   .arg(dropped == 1 ? "" : "s");
    }
    if (!plan.launcher_dir.empty()) lines << "Deletes " + name + " itself.";
    if (plan.signs_out) lines << "Signs you out of " + name + ".";
    if (!plan.kept.empty()) lines << "Keeps game data and saves (prefixes stay on disk).";
    lines << name + " is turned off; turn it on again on the Sources page any time.";
    if (!notify::Confirm(parent, "Remove " + name, lines.join("\n"), "Remove",
                         /*destructive=*/true))
      return;
    api::RemoveSourceAsync(parent, id, [parent, name, on_removed](RemoveSourceResult r) {
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
    });
  });
}

}  // namespace mira_gui
