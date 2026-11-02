#pragma once

#include <QString>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../settings/SettingsCard.h"

class QLabel;
class QPushButton;

namespace mira_gui {

struct SourceInfo;

// Asks before removing a mod manager from a game, saying what goes by what mirad found (GET
// /v1/games/{id}/mod-managers/{manager}): Vortex's files deployed into the game's folders, the
// game files it set aside, and its mods, downloads and settings for the game. Remove stays off
// until that's known, and while the manager is open. An in-window card (cards::Show).
class RemoveModManagerCard : public SettingsCard {
  Q_OBJECT

 public:
  RemoveModManagerCard(const QString& game_name, const SourceInfo& manager,
                       QWidget* parent = nullptr);

  // What removing it would do; until it's set every row shows "…".
  void SetSummary(const ModManagerPurgeResult& summary);

 signals:
  void Confirmed();

 private:
  // A row with a label and muted text on the right, which `text` fills again with the summary.
  void AddInfoRow(const QString& label, std::function<QString()> text,
                  std::function<QString()> tooltip = nullptr);

  QString manager_name_;
  bool vortex_ = false;
  std::optional<ModManagerPurgeResult> summary_;
  std::vector<std::function<void()>> refresh_;
  QLabel* note_ = nullptr;
  QPushButton* confirm_ = nullptr;
};

}  // namespace mira_gui
