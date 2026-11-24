#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../components/SheetCard.h"

class QLabel;
class QPushButton;

namespace mira_gui {

class ArtworkStore;
struct GameSummary;

// After an install left its installer folder behind (game.installer_leftover): delete it
// (DELETE /v1/games/{id}/installer) or keep it.
class InstallerLeftoverCard : public SheetCard {
  Q_OBJECT

 public:
  InstallerLeftoverCard(const GameSummary& game, const std::string& installer_dir,
                        std::int64_t bytes, ArtworkStore* artwork, QWidget* parent = nullptr);

 signals:
  void CloseRequested();

 private:
  std::string game_id_;
  QLabel* error_ = nullptr;
  QPushButton* delete_ = nullptr;
};

// Offers the programs a run in the game's prefix installed as its launch options, a row each.
class ProgramsFoundCard : public SheetCard {
  Q_OBJECT

 public:
  // `current` gives the game's launch options as the library has them now.
  ProgramsFoundCard(const GameSummary& game, const ProgramsFoundEvent& event, ArtworkStore* artwork,
                    std::function<GameLaunch()> current, QWidget* parent = nullptr);

 signals:
  void CloseRequested();

 private:
  std::string game_id_;
  std::function<GameLaunch()> current_;
  std::vector<LaunchOption> added_;
  std::vector<QPushButton*> adds_;
  QLabel* error_ = nullptr;
};

}  // namespace mira_gui
