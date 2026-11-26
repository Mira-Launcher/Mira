#pragma once

#include <QPointer>
#include <QWidget>
#include <string>
#include <vector>

#include "../client/Types.h"

class QLabel;
class QNetworkAccessManager;
class QScrollArea;

namespace mira_gui {

class Card;
class ColumnGrid;

// What a game is, from its store's cached info (GET /v1/games/{id}/metadata), as a page in the game
// card's place: an About card (facts, the description, review, ProtonDB and content chips, the
// website), then the screenshots (fetched only once the page opens) and the system requirements.
class StoreInfoPage : public QWidget {
  Q_OBJECT

 public:
  StoreInfoPage(std::string id, QWidget* parent = nullptr);

  // Reads the store info again, as when the game's identity changed.
  void Load();
  void SetColumnWidth(int width);

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  void Show(const GameMetadataResult& result);
  void AddScreenshots(const std::vector<std::string>& urls);
  // A screenshot large over the page, until clicked.
  void ShowLarge(const QPixmap& pixmap);

  std::string id_;
  int request_ = 0;
  QScrollArea* scroll_ = nullptr;
  ColumnGrid* grid_ = nullptr;
  QLabel* state_ = nullptr;  // loading, none yet, or the error
  Card* about_ = nullptr;
  Card* screenshots_ = nullptr;
  Card* requirements_ = nullptr;
  QNetworkAccessManager* network_ = nullptr;
  QPointer<QLabel> large_;
};

}  // namespace mira_gui
