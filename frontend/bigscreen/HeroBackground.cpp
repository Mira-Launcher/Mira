#include "HeroBackground.h"

#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QScreen>
#include <QUrl>
#include <QVideoFrame>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "../client/api/Artwork.h"
#include "../library/CoverArt.h"
#include "../theme/Theme.h"
#include "BigScreenWindow.h"
#include "Paint.h"

namespace mira_gui::bigscreen {

namespace {

// The focus color from the art's bright, saturated pixels; invalid when the art is grey.
QColor ArtAccent(const QPixmap& art) {
  if (art.isNull()) return {};
  const QImage image = art.toImage().scaled(24, 24, Qt::IgnoreAspectRatio, Qt::FastTransformation);
  double x = 0, y = 0, sat = 0, val = 0, total = 0;
  for (int py = 0; py < image.height(); ++py) {
    for (int px = 0; px < image.width(); ++px) {
      const QColor c = image.pixelColor(px, py).toHsv();
      const double v = c.valueF();
      if (v < 0.25) continue;
      const double s = c.saturationF();
      const double w = s * v;
      const double angle = std::max(0.0, static_cast<double>(c.hsvHueF())) * 2 * std::numbers::pi;
      x += w * std::cos(angle);
      y += w * std::sin(angle);
      sat += w * s;
      val += w * v;
      total += w;
    }
  }
  if (total < 1.0) return {};
  const double hue = std::fmod(std::atan2(y, x) / (2 * std::numbers::pi) + 1, 1.0);
  return QColor::fromHsvF(hue, std::clamp(sat / total, 0.45, 0.85), std::clamp(val / total, 0.75, 0.95));
}

}  // namespace

HeroBackground::HeroBackground(BigScreenWindow* window) : QWidget(window), window_(window) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  fade_.setDuration(450);
  fade_.setStartValue(0.0);
  fade_.setEndValue(1.0);
  connect(&fade_, &QVariantAnimation::valueChanged, this, [this] { update(); });
  // Fetched once focus rests, not for every game scrolled past.
  load_.setSingleShot(true);
  load_.setInterval(150);
  connect(&load_, &QTimer::timeout, this, &HeroBackground::LoadHero);

  trailer_fade_.setDuration(600);
  trailer_fade_.setStartValue(0.0);
  trailer_fade_.setEndValue(1.0);
  connect(&trailer_fade_, &QVariantAnimation::valueChanged, this, [this] { update(); });
  connect(&trailer_fade_, &QVariantAnimation::finished, this, [this] {
    if (trailer_fade_.direction() != QAbstractAnimation::Backward) return;
    frame_ = {};
    update();
  });

  trailer_delay_.setSingleShot(true);
  trailer_delay_.setInterval(3000);
  connect(&trailer_delay_, &QTimer::timeout, this, [this] {
    if (trailer_key_.isEmpty() || !window_->isActiveWindow()) return;
    const QString key = trailer_key_;
    api::GetMetadataAsync(this, key.toStdString(), [this, key](GameMetadataResult result) {
      if (key != trailer_key_ || !result.ok || result.metadata.trailers.empty()) return;
      CreatePlayer();
      player_->setSource(QUrl(QString::fromStdString(result.metadata.trailers[0])));
      player_->play();
    });
  });
}

void HeroBackground::Show(const Item& item) {
  if (item.key == key_) return;
  key_ = item.key;
  trailer_delay_.stop();
  if (player_ != nullptr) player_->stop();
  frame_ = {};
  trailer_fade_.stop();
  trailer_fade_.setDirection(QAbstractAnimation::Forward);
  trailer_fade_.setCurrentTime(0);
  const bool trailers = window_->prefs().big_screen_trailers.value_or(true);
  trailer_key_ = item.game && trailers ? key_ : QString();
  if (!trailer_key_.isEmpty()) trailer_delay_.start();

  const auto hero = heroes_.constFind(key_);
  if (hero != heroes_.constEnd() && !logos_.contains(key_)) LoadLogo(key_);
  if (hero != heroes_.constEnd() && !hero->isNull()) return Fade(*hero);
  Fade(Blurred(item));
  if (hero == heroes_.constEnd()) load_.start();
}

void HeroBackground::Fade(const QPixmap& art) {
  previous_ = current_;
  current_ = art;
  SetAccent(ArtAccent(current_));
  emit ArtChanged();
  fade_.stop();
  fade_.start();
}

void HeroBackground::LoadHero() {
  const QString key = key_;
  LoadLogo(key);
  // ArtworkStore keeps only a small copy of most heroes, too coarse for a whole screen.
  // Decoded at the screen's size: a 4K hero is 33 MB unpacked.
  api::GetArtworkImageAsync(
      this, key.toStdString(), "hero",
      [this, key](QImage image) {
        if (heroes_.size() >= kKeepHeroes) heroes_.clear();
        const QPixmap hero = image.isNull() ? QPixmap() : QPixmap::fromImage(std::move(image));
        heroes_.insert(key, hero);
        if (key == key_ && !hero.isNull()) Fade(hero);
      },
      ScreenSize());
}

void HeroBackground::LoadLogo(const QString& key) {
  if (logos_.contains(key)) return;
  // Drawn at most a few rows tall.
  api::GetArtworkImageAsync(
      this, key.toStdString(), "logo",
      [this, key](QImage image) {
        if (logos_.size() >= kKeepLogos) logos_.clear();
        logos_.insert(key, image.isNull() ? QPixmap() : QPixmap::fromImage(std::move(image)));
        emit ArtChanged();
      },
      QSize(ScreenSize().width(), ScreenSize().height() / 4));
}

// Made on the first trailer: loading Qt's media backend pulls in FFmpeg, which big screen
// shouldn't depend on just to open.
void HeroBackground::CreatePlayer() {
  if (player_ != nullptr) return;
  player_ = new QMediaPlayer(this);
  sink_ = new QVideoSink(this);
  audio_ = new QAudioOutput(this);
  audio_->setMuted(true);
  audio_->setVolume(0);
  player_->setAudioOutput(audio_);
  player_->setVideoSink(sink_);
  connect(sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
    if (trailer_key_ != key_) return;
    QImage image = frame.toImage();
    if (image.isNull()) return;
    const bool first = frame_.isNull();
    frame_ = std::move(image);
    if (first) {
      trailer_fade_.setDirection(QAbstractAnimation::Forward);
      trailer_fade_.start();
    }
    update();
  });
  connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
    if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia) EndTrailer();
  });
  connect(player_, &QMediaPlayer::errorOccurred, this, [this] { EndTrailer(); });
  // Trailers play only while the window has focus.
  connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
    const bool active = window_->isActiveWindow();
    if (!active && player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();
    if (active && player_->playbackState() == QMediaPlayer::PausedState) player_->play();
  });
}

void HeroBackground::EndTrailer() {
  if (player_ != nullptr) player_->stop();
  trailer_fade_.setDirection(QAbstractAnimation::Backward);
  trailer_fade_.start();
}

// The screen's size in pixels: the widget's own size is still the default until big screen
// goes full screen, which can be after the first hero is fetched.
QSize HeroBackground::ScreenSize() const {
  const QScreen* shown = window_->screen();
  return shown == nullptr ? QSize() : shown->size() * shown->devicePixelRatio();
}

QPixmap HeroBackground::Logo(const QString& key) const { return logos_.value(key); }

QPixmap HeroBackground::Blurred(const Item& item) const {
  if (item.key.isEmpty()) return {};
  // A slice of a portrait cover reads as noise; a wash of its colors doesn't.
  const QPixmap cover = window_->Cover(item, QSize(200, 300));
  return cover.isNull() ? QPixmap() : BlurredCover(cover, QSize(1600, 900));
}

void HeroBackground::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  const QColor base = theme::Current().window;
  painter.fillRect(rect(), base);
  // Fill the screen, cropping the longer side around the center.
  const auto fill = [&](QSizeF source) {
    const QSizeF scaled = source.scaled(size(), Qt::KeepAspectRatioByExpanding);
    return QRectF((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled.width(),
                  scaled.height());
  };
  const auto draw = [&](const QPixmap& art, double opacity) {
    if (art.isNull() || opacity <= 0) return;
    painter.setOpacity(opacity);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(fill(art.size()), art, QRectF(art.rect()));
  };
  const double t = fade_.state() == QAbstractAnimation::Running ? fade_.currentValue().toDouble() : 1.0;
  draw(previous_, 1.0 - t);
  draw(current_, t);
  painter.setOpacity(1.0);

  if (!frame_.isNull()) {
    painter.setOpacity(trailer_fade_.currentValue().toDouble());
    painter.drawImage(fill(frame_.size()), frame_);
    painter.setOpacity(1.0);
  }

  QColor solid = base, clear = base;
  clear.setAlpha(0);
  QLinearGradient left(0, 0, width(), 0);
  QColor heavy = base, mid = base;
  heavy.setAlpha(242);
  mid.setAlpha(190);
  left.setColorAt(0, heavy);
  left.setColorAt(0.38, mid);
  left.setColorAt(0.75, clear);
  painter.fillRect(rect(), left);
  QLinearGradient bottom(0, height(), 0, 0);
  QColor dense = base;
  dense.setAlpha(230);
  bottom.setColorAt(0, solid);
  bottom.setColorAt(0.42, dense);
  bottom.setColorAt(0.75, clear);
  painter.fillRect(rect(), bottom);
}

}  // namespace mira_gui::bigscreen
