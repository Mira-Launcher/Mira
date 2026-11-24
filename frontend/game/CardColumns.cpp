#include "CardColumns.h"

namespace mira_gui {

CardColumns CardColumnsFor(const GameDetail& game, const QString& store) {
  CardColumns columns;
  if (game.source == "steam") {
    columns.runs = {"Steam", "Proton"};
  } else if (game.runner_ref.starts_with("emulator:")) {
    columns.runs = {"Emulator"};
  } else {
    columns.runs = {"Runner"};
    if (!store.isEmpty()) columns.runs.push_back(store);
  }
  for (const char* title : {"Display", "Performance and overlay", "Logs"})
    columns.runs.emplace_back(title);
  columns.game = {"Details"};
  if (game.source != "steam" && !game.runner_ref.starts_with("emulator:"))
    columns.game.emplace_back("Launch");
  for (const char* title : {"Saves", "Scripts", "Files", "Menu entries"})
    columns.game.emplace_back(title);
  return columns;
}

bool ShowsPrefixRow(const GameDetail& game) {
  const bool own_files = game.source != "desktop-entry" && game.platform != "web";
  return own_files && game.platform != "native" && !game.runner_ref.starts_with("emulator:") &&
         (game.source != "steam" || !game.data_dir.empty());
}

}  // namespace mira_gui
