#pragma once

#include <QString>
#include <optional>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {

// One row of the Updates panel. A store's tool and helper come as one, since its setup updates
// both.
struct UpdateRow {
  std::string kind;  // "emulator", "mod_manager", "tool" or "runner"
  std::string id;
  QString name;
  QString detail;         // "0.20.34 → 0.20.37"
  std::string installed;  // a runner's label, for asking to remove it afterwards
  std::string release;    // a runner's new release
};

struct UpdateGroup {
  QString heading;
  std::vector<UpdateRow> rows;
};

// Everything but Mira itself, in the panel's order; groups with no rows are left out.
std::vector<UpdateGroup> GroupUpdates(const std::vector<UpdateInfo>& updates);

// Mira's own entry, unless this is a build of the user's own.
std::optional<UpdateInfo> MiraEntry(const std::vector<UpdateInfo>& updates);

}  // namespace mira_gui
