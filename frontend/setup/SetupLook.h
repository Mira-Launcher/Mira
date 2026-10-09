#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QString>
#include <QWidget>
#include <functional>
#include <vector>

#include "SetupWindow.h"

class QButtonGroup;
class QLabel;
class QNetworkAccessManager;

namespace mira_gui {

class Switch;

// Three example games' covers and heroes from Steam's CDN, for previews before the user has games
// of their own. Held in memory only, gone when the setup window closes.
class DemoArt : public QObject {
  Q_OBJECT

 public:
  struct Game {
    QString appid;
    QString name;
    int days_ago = 0;  // when it was last played, for the sidebar preview
  };
  static const std::vector<Game>& Games();

  explicit DemoArt(QObject* parent);
  // Null until fetched, or when offline.
  QPixmap Cover(int index) const { return covers_.value(index); }
  QPixmap Hero(int index) const { return heroes_.value(index); }

 signals:
  void Loaded();

 private:
  QNetworkAccessManager* network_ = nullptr;
  QHash<int, QPixmap> covers_;
  QHash<int, QPixmap> heroes_;
};

// One game or app as a preview draws it: its art when there is some, else its color and letter.
struct Picture {
  QPixmap cover;
  QPixmap hero;
  QString name;
  QString when;        // as the sidebar says it was last played: "Yesterday"
  QString short_when;  // the same on a shelf cover: "1d ago"
  QString source;      // the store or launcher's name
  QColor color;
  QString letter;
};

// The user's most recently played games, else the example ones.
std::vector<Picture> PreviewGames(const SetupContext& context, int count, bool heroes = false);
// The Microsoft 365 apps picked, else Word, Excel and PowerPoint.
std::vector<Picture> PreviewApps(const SetupContext& context);
// An Office app's own color, for its tile.
QColor OfficeAppColor(const QString& ref);

// Covers fanned out like a hand of cards, redrawn as their art arrives.
class CoverFan : public QWidget {
  Q_OBJECT

 public:
  CoverFan(const SetupContext& context, std::function<std::vector<Picture>()> pictures,
           QSize card, int step, qreal turn, QWidget* parent);
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  std::function<std::vector<Picture>()> pictures_;
  QSize card_;
  int step_;
  qreal turn_;
};

// Theme, corners, spacing and what tiles show, over a small library drawn the way they make it.
class LookPage : public SetupStep {
  Q_OBJECT

 public:
  explicit LookPage(const SetupContext& context);
  void Enter() override;

 private:
  void Save(const FrontendPrefs& change);

  const SetupContext& context_;
  QButtonGroup* themes_ = nullptr;
  QButtonGroup* corners_ = nullptr;
  QButtonGroup* spacing_ = nullptr;
  QWidget* games_switches_ = nullptr;
  QWidget* apps_switches_ = nullptr;
  Switch* continue_ = nullptr;
  Switch* recent_apps_ = nullptr;
  Switch* status_ = nullptr;
  Switch* app_status_ = nullptr;
  Switch* source_ = nullptr;
  QWidget* preview_ = nullptr;
};

// How the sidebar draws pinned and recently played games, beside a sidebar drawn that way.
class SidebarPage : public SetupStep {
  Q_OBJECT

 public:
  explicit SidebarPage(const SetupContext& context);
  void Enter() override;
  void Leave() override;

 private:
  void Save(const FrontendPrefs& change);

  const SetupContext& context_;
  QButtonGroup* pinned_ = nullptr;
  QButtonGroup* recent_ = nullptr;
  QWidget* games_rows_ = nullptr;
  Switch* when_ = nullptr;
  Switch* covers_ = nullptr;
  QLabel* lead_ = nullptr;
  QLabel* hint_ = nullptr;
  QWidget* preview_ = nullptr;
};

}  // namespace mira_gui
