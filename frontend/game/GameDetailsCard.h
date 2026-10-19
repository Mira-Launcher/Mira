#pragma once

#include <QPointer>
#include <functional>
#include <string>

#include "../client/Types.h"
#include "HeroBackdrop.h"

class QHBoxLayout;
class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class GameLibraryModel;

// What a game is, from its store's cached info (GET /v1/games/{id}/metadata), as an in-window
// card (cards::Show): the hero with the cover, name, status and Play, then a facts line, the
// description, review, ProtonDB and content chips, screenshots as pictures (fetched only while the
// card shows), the system requirements and the website. The game menu's Game details….
class GameDetailsCard : public HeroBackdrop {
  Q_OBJECT

 public:
  // Reads the store info first, so the card opens at its size: the hero alone without any.
  // `play` starts or stops the game, as the tile's Play does.
  static void Open(QWidget* from, const std::string& id, GameLibraryModel* library,
                   ArtworkStore* artwork, std::function<void()> play);

 private:
  GameDetailsCard(const std::string& id, const GameMetadataResult& metadata,
                  GameLibraryModel* library, ArtworkStore* artwork, std::function<void()> play,
                  QWidget* parent = nullptr);

  void UpdateIdentity();
  void ShowMetadata(const GameMetadataResult& result);
  void AddScreenshots(const std::vector<std::string>& urls);
  // A screenshot large over the card, until clicked.
  void ShowLarge(const QPixmap& pixmap);

  std::string id_;
  GameLibraryModel* library_ = nullptr;
  QLabel* title_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* play_ = nullptr;
  QWidget* panel_ = nullptr;
  // Whether the info has More or the requirements to open.
  bool expandable_ = false;
  QVBoxLayout* body_ = nullptr;
  QNetworkAccessManager* network_ = nullptr;
  QPointer<QLabel> large_;
};

}  // namespace mira_gui
