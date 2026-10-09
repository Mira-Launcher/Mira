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
  // Another page came to the front: the trailer stops, and the next Show starts over even for
  // the same game. The hero stays until then.
  void Leave();
  // The game's logo art (transparent PNG), once fetched; null when it has none or isn't loaded.
  QPixmap Logo(const QString& key) const;
  // Focus has rested on the game for the start delay: pages step back to show its hero, and its
  // trailer when it has one.
  bool Showcasing() const { return reveal_; }

signals:
  // A logo arrived, or the accent color changed with the focused game.
  void ArtChanged();
  // Showcasing() changed.
  void ShowcaseChanged();

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
  // Stops the trailer at once (fading its sound out when it has any) and clears its picture.
  void StopTrailer();

  static constexpr int kKeepHeroes = 4;
  static constexpr int kKeepLogos = 8;
  // Trailers this short start at the beginning, whatever the skip.
  static constexpr qint64 kSkipFromMs = 20000;
  // How long focus rests before a trailer starts loading. Loading stalls the GUI thread for a
  // tenth of a second or more, so it waits until the hero and Home's rows have stopped moving.
  static constexpr int kLoadAfterMs = 1000;

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
  // Loads the trailer a moment after focus lands, so scrolling past fetches nothing.
  QTimer trailer_delay_;
  QImage frame_;
  // The first frame, blurred: the trailer fades in through it.
  QPixmap frame_blur_;
  // The game the player was loaded for; frames from an earlier one are dropped.
  QString playing_key_;
  // Lowers the sound before a trailer that's left stops, instead of cutting it.
  QVariantAnimation volume_fade_;
  qint64 skip_us_ = 0;  // frames before this are not drawn
  // Shows the trailer once focus has rested for the start delay.
  QTimer trailer_reveal_;
  bool reveal_ = false;
  QVariantAnimation trailer_fade_;
  // The game whose trailer is wanted; empty when none.
  QString trailer_key_;
};

}  // namespace mira_gui::bigscreen
