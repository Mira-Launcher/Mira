#pragma once

#include <QAudioOutput>
#include <QHash>
#include <QImage>
#include <QMediaPlayer>
#include <QPixmap>
#include <QTimer>
#include <QVariantAnimation>
#include <QVideoSink>
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
  // The game's logo art (transparent PNG), once fetched; null when it has none or isn't loaded.
  QPixmap Logo(const QString& key) const;

signals:
  // A logo arrived, or the accent color changed with the focused game.
  void ArtChanged();

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QPixmap Blurred(const Item& item) const;
  void Fade(const QPixmap& art);
  void LoadHero();
  void LoadLogo(const QString& key);
  QSize ScreenSize() const;
  void CreatePlayer();
  void EndTrailer();

  static constexpr int kKeepHeroes = 4;
  static constexpr int kKeepLogos = 8;

  BigScreenWindow* window_;
  QString key_;
  // Full heroes fetched lately, by key; a null one means it has none.
  QHash<QString, QPixmap> heroes_;
  // Logos fetched lately, by key; a null one means it has none.
  QHash<QString, QPixmap> logos_;
  QTimer load_;
  QPixmap current_;
  QPixmap previous_;
  QVariantAnimation fade_;

  QMediaPlayer* player_ = nullptr;
  QVideoSink* sink_ = nullptr;
  QAudioOutput* audio_ = nullptr;
  // Starts the trailer after a short wait, so scrolling past doesn't play it.
  QTimer trailer_delay_;
  QImage frame_;
  QVariantAnimation trailer_fade_;
  // The game whose trailer is wanted; empty when none.
  QString trailer_key_;
};

}  // namespace mira_gui::bigscreen
