#pragma once

#include <QString>
#include <optional>
#include <string>

#include "../client/Types.h"
#include "LiveCard.h"

class QLabel;
class QPushButton;

namespace mira_gui {

struct SourceInfo;

// Asks before removing a mod manager from a game, saying what goes by what mirad found (GET
// /v1/games/{id}/mod-managers/{manager}): Vortex's files deployed into the game's folders, the
// game files it set aside, and its mods, downloads and settings for the game. Remove stays off
// until that's known, and while the manager is open. An in-window card (cards::Show).
class RemoveModManagerCard : public LiveCard {
  Q_OBJECT

 public:
  RemoveModManagerCard(const QString& game_name, const SourceInfo& manager,
                       QWidget* parent = nullptr);

  // What removing it would do; until it's set every row shows "…".
  void SetSummary(const ModManagerPurgeResult& summary);

 signals:
  void Confirmed();

 private:
  QString manager_name_;
  bool vortex_ = false;
  std::optional<ModManagerPurgeResult> summary_;
  QLabel* note_ = nullptr;
  QPushButton* confirm_ = nullptr;
};

}  // namespace mira_gui
