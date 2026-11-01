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

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  QWidget* MakeCard(const GameSummary& game, bool running);
  // Shows the cards that fit the row's width, at least one; the rest wait for a wider window.
  void FitCards();

  ArtworkStore* artwork_ = nullptr;
  QHBoxLayout* cards_ = nullptr;
  std::vector<GameSummary> shown_;
  std::map<std::string, QLabel*> covers_;  // by game id
};

}  // namespace mira_gui
