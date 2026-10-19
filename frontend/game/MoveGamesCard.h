#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../settings/SettingsCard.h"

class QPushButton;
class QLabel;

namespace mira_gui {

// Asks before moving games into Mira's folders, by the plans mirad gave (POST
// /v1/games/relocation): for one game, where its files and prefix go (or why they stay); for
// several, a row per game that opens to the same lines, with the games that have nothing to move
// sharing one. An in-window card (cards::Show).
class MoveGamesCard : public SettingsCard {
  Q_OBJECT

 public:
  explicit MoveGamesCard(std::vector<RelocationPlan> plans, QWidget* parent = nullptr);

  // The sizes behind the plans, once measured; until then they show as "…".
  void SetSizes(const std::map<std::string, RemovalSize>& sizes);

  // Whether moving `plan`'s game changes anything.
  static bool Moves(const RelocationPlan& plan);

 signals:
  // Move these games.
  void Confirmed(std::vector<std::string> ids);

 private:
  // A game's two lines, files and prefix, under `parent`'s layout.
  void AddPartLines(QWidget* parent, const RelocationPlan& plan);
  QString MovingSize(const RelocationPlan& plan) const;

  std::vector<RelocationPlan> plans_;
  std::map<std::string, RemovalSize> sizes_;
  bool measured_ = false;
  std::vector<std::function<void()>> refresh_;
};

}  // namespace mira_gui
