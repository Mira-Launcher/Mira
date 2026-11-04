#include "UpdateRows.h"

#include <algorithm>

namespace mira_gui {
namespace {

QString Detail(const UpdateInfo& update) {
  const QString installed = QString::fromStdString(update.installed);
  const QString latest = QString::fromStdString(update.latest);
  // A release that keeps its tag, such as DuckStation's "latest".
  if (installed == latest) return "A newer build of " + latest;
  if (installed.isEmpty()) return "Adds " + latest;
  return installed + " → " + latest;
}

}  // namespace

std::vector<UpdateGroup> GroupUpdates(const std::vector<UpdateInfo>& updates) {
  std::vector<UpdateGroup> groups = {
      {"WINE AND PROTON", {}}, {"EMULATORS", {}}, {"MOD MANAGERS", {}}, {"TOOLS", {}}};
  const auto group_of = [](const std::string& kind) {
    if (kind == "runner") return 0;
    if (kind == "emulator") return 1;
    return kind == "mod_manager" ? 2 : 3;
  };
  for (const UpdateInfo& update : updates) {
    if (update.kind == "mira") continue;
    auto& rows = groups[group_of(update.kind)].rows;
    const auto same = std::ranges::find_if(rows, [&update](const UpdateRow& row) {
      return row.kind == update.kind && row.id == update.id;
    });
    const QString name = QString::fromStdString(update.name);
    if (same != rows.end()) {
      same->name += " and " + name;
      same->detail += "  ·  " + name + " " + Detail(update);
      continue;
    }
    rows.push_back({.kind = update.kind,
                    .id = update.id,
                    .name = name,
                    .detail = Detail(update),
                    .installed = update.installed,
                    .release = update.release});
  }
  std::erase_if(groups, [](const UpdateGroup& group) { return group.rows.empty(); });
  return groups;
}

std::optional<UpdateInfo> MiraEntry(const std::vector<UpdateInfo>& updates) {
  const auto mira = std::ranges::find(updates, std::string("mira"), &UpdateInfo::kind);
  if (mira == updates.end() || mira->install == "none") return std::nullopt;
  return *mira;
}

}  // namespace mira_gui
