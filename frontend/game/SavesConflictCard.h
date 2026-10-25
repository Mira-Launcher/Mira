#pragma once

#include <string>

#include "../settings/SettingsCard.h"

class QLabel;

namespace mira_gui {

class ArtworkStore;
class LaidOutButton;
struct GameSummary;

// One side of a cloud save conflict as a choice card: `name`, what it holds (`detail`, kept in
// `detail_label` for filling in later) and what picking it does.
LaidOutButton* MakeSavesChoice(QWidget* parent, const QString& name, const QString& detail,
                               const QString& effect, QLabel** detail_label = nullptr);

// Asked when a launch found the game's saves changed both here and in its store's cloud: which
// to start with. An in-window card over the library.
class SavesConflictCard : public SettingsCard {
  Q_OBJECT

 public:
  SavesConflictCard(const GameSummary& game, ArtworkStore* artwork, QWidget* parent = nullptr);

 signals:
  // "keep_local", "use_cloud" or "skip"; "" when the launch is cancelled.
  void Chosen(std::string choice);

 private:
  std::string id_;
  QLabel* local_ = nullptr;  // when this computer's were saved, and what they hold
  QLabel* cloud_ = nullptr;  // when the cloud's were uploaded
};

}  // namespace mira_gui
