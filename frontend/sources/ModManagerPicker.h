#pragma once

#include <QString>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../library/GamePicker.h"

namespace mira_gui {

// The Windows games a mod manager isn't on yet, to tick the ones to add it to. Taking it off a
// game purges its mods, so that stays on the game's page.
class ModManagerPicker : public GamePicker {
  Q_OBJECT

 public:
  ModManagerPicker(ArtworkStore* artwork, const QString& manager_name, QWidget* parent = nullptr);

  // `games` are the ones it can go on.
  void Open(const std::vector<const GameSummary*>& games);

 signals:
  // Add it to these games, by id.
  void Picked(const std::vector<std::string>& ids);

 protected:
  void UpdateApply() override;
  void Apply() override;
};

}  // namespace mira_gui
