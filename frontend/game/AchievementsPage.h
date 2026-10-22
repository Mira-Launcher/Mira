#pragma once

#include <QFrame>
#include <QPixmap>
#include <QPointer>
#include <QWidget>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "../client/Types.h"

class QGridLayout;
class QLabel;
class QScrollArea;

namespace mira_gui {

class AchievementIcon;
class ProgressRail;

// One achievement: its icon, name, description, when it was unlocked and how rare it is. A hidden
// one that's still locked shows as a hidden achievement until clicked.
class AchievementTile : public QFrame {
  Q_OBJECT

 public:
  AchievementTile(const Achievement& item, QWidget* parent = nullptr);

  const Achievement& item() const { return item_; }
  bool Masked() const;
  void SetIcon(const QPixmap& icon);

 signals:
  // A hidden one was clicked and now shows what it is.
  void Revealed();

 protected:
  void mouseReleaseEvent(QMouseEvent* event) override;

 private:
  void Fill();

  Achievement item_;
  bool revealed_ = false;
  QPixmap pixmap_;
  AchievementIcon* icon_ = nullptr;
  QLabel* name_ = nullptr;
  QLabel* description_ = nullptr;
  QLabel* when_ = nullptr;
  QLabel* rarity_ = nullptr;
};

// The game card's Achievements page: the count, then every achievement as a tile, in the store's
// order. Icons load as the page shows them and go with it.
class AchievementsPage : public QWidget {
  Q_OBJECT

 public:
  explicit AchievementsPage(std::string id, QWidget* parent = nullptr);

  // Shows `set`, keeping the icons already loaded for items whose pictures didn't change.
  void Show(const AchievementsResult& set);

 signals:
  void BackClicked();

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  // Places the tiles in as many columns as fit.
  void Arrange();
  void LoadIcon(AchievementTile* tile);
  // Starts queued icon requests while fewer than kIconRequests are out.
  void NextIcons();

  std::string id_;
  // Each request holds a thread here and in mirad until answered, so a page of 200 asks a few
  // at a time.
  static constexpr int kIconRequests = 4;
  std::deque<QPointer<AchievementTile>> icon_queue_;
  int icon_requests_ = 0;
  QLabel* count_ = nullptr;
  ProgressRail* rail_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QGridLayout* grid_ = nullptr;
  std::vector<AchievementTile*> tiles_;
  int columns_ = 0;
  // Loaded icons by key, state and version, so a refresh that changes nothing reloads none.
  std::map<std::string, QPixmap> icons_;
};

}  // namespace mira_gui
