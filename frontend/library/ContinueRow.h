#pragma once

#include <QPoint>
#include <QString>
#include <QWidget>

#include <map>
#include <string>
#include <vector>

#include "../client/Types.h"

class QHBoxLayout;
class QLabel;

namespace mira_gui {

class ArtworkStore;

// Large cards above the library grid: running games first, then the most
// recently played.
class ContinueRow : public QWidget {
  Q_OBJECT

public:
  ContinueRow(ArtworkStore* artwork, QWidget* parent = nullptr);

  void SetGames(const std::vector<const GameSummary*>& games);
  // Redraws `id`'s cover if it has a card.
  void RefreshCover(const std::string& id);

signals:
  void PlayToggled(QString id);
  void MenuRequested(QString id, QPoint global_pos);

private:
  QWidget* MakeCard(const GameSummary& game, bool running);

  ArtworkStore* artwork_ = nullptr;
  QHBoxLayout* cards_ = nullptr;
  std::vector<GameSummary> shown_;
  std::map<std::string, QLabel*> covers_;  // by game id
};

}  // namespace mira_gui
