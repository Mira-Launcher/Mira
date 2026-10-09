#pragma once

#include <QHash>
#include <QPixmap>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

namespace mira_gui::bigscreen {

class BigScreenWindow;
struct Item;

// The focused game's hero across the whole screen, behind every page, with
// scrims so text on the left and the rows at the bottom stay readable. A game
// with no hero gets its cover blurred into a wash of color.
class HeroBackground : public QWidget {
  Q_OBJECT

public:
  explicit HeroBackground(BigScreenWindow* window);

  void Show(const Item& item);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QPixmap Blurred(const Item& item) const;
  void Fade(const QPixmap& art);
  void LoadHero();

  BigScreenWindow* window_;
  QString key_;
  // Full heroes fetched lately, by key; a null one means it has none.
  QHash<QString, QPixmap> heroes_;
  QTimer load_;
  QPixmap current_;
  QPixmap previous_;
  QVariantAnimation fade_;
};

}  // namespace mira_gui::bigscreen
