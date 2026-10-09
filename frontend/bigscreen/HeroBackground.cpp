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
  fade_.setDuration(500);
  fade_.setEasingCurve(QEasingCurve::OutCubic);
  fade_.setStartValue(0.0);
  fade_.setEndValue(1.0);
  connect(&fade_, &QVariantAnimation::valueChanged, this, [this] { update(); });
  // Fetched once focus rests, not for every game scrolled past.
  load_.setSingleShot(true);
  load_.setInterval(150);
  connect(&load_, &QTimer::timeout, this, &HeroBackground::LoadHero);

  trailer_fade_.setDuration(900);
  trailer_fade_.setStartValue(0.0);
  trailer_fade_.setEndValue(1.0);
  connect(&trailer_fade_, &QVariantAnimation::valueChanged, this, [this] { update(); });
  connect(&trailer_fade_, &QVariantAnimation::finished, this, [this] {
    if (trailer_fade_.direction() != QAbstractAnimation::Backward) return;
    frame_ = {};
    frame_blur_ = {};
    update();
  });
  volume_fade_.setDuration(300);
  volume_fade_.setEndValue(0.0);
  connect(&volume_fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    if (audio_ != nullptr) audio_->setVolume(float(value.toDouble()));
  });
  connect(&volume_fade_, &QVariantAnimation::finished, this, [this] {
    if (player_ != nullptr && playing_key_ != key_) player_->stop();
  });

  // Loaded soon after focus lands, then held unseen until the start delay has passed.
  trailer_delay_.setSingleShot(true);
  connect(&trailer_delay_, &QTimer::timeout, this, [this] {
    if (trailer_key_.isEmpty() || !window_->isActiveWindow()) return;
    const QString key = trailer_key_;
    api::GetMetadataAsync(this, key.toStdString(), [this, key](GameMetadataResult result) {
      if (key != trailer_key_ || !result.ok || result.metadata.trailers.empty()) return;
      CreatePlayer();
      const FrontendPrefs& prefs = window_->prefs();
      skip_us_ = std::max(0, prefs.big_screen_trailer_skip_ms.value_or(3000)) * qint64{1000};
      volume_fade_.stop();
      playing_key_ = key;
      audio_->setMuted(true);  // until it shows
      audio_->setVolume(std::clamp(prefs.big_screen_trailer_volume.value_or(50), 0, 100) / 100.0);
      player_->setSource(QUrl(QString::fromStdString(result.metadata.trailers[0])));
      // The opening plays through fast and unseen: seeking a stream takes seconds.
      player_->setPlaybackRate(skip_us_ > 0 ? 4.0 : 1.0);
      player_->play();
    });
  });
  // A trailer that couldn't load while big screen was in the background loads once it's back.
  connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
    if (window_->isActiveWindow() && !trailer_key_.isEmpty() && trailer_key_ == key_ && playing_key_ != key_ &&
        !trailer_delay_.isActive()) {
      trailer_delay_.start(kLoadAfterMs);
    }
  });
  trailer_reveal_.setSingleShot(true);
  connect(&trailer_reveal_, &QTimer::timeout, this, [this] {
    reveal_ = true;
    emit ShowcaseChanged();
    if (player_ != nullptr && trailer_key_ == key_ && player_->playbackState() == QMediaPlayer::PausedState &&
        window_->isActiveWindow()) {
      player_->play();
    }
  });
}

void HeroBackground::Leave() {
  if (key_.isEmpty()) return;
  key_.clear();
  trailer_key_.clear();
  StopTrailer();
  trailer_reveal_.stop();
  if (std::exchange(reveal_, false)) emit ShowcaseChanged();
  update();
}

void HeroBackground::StopTrailer() {
  trailer_delay_.stop();
  playing_key_.clear();  // its frames are dropped from here on
  // A trailer with sound fades out; a silent one just stops.
  if (player_ != nullptr && player_->playbackState() == QMediaPlayer::PlayingState && !audio_->isMuted()) {
    volume_fade_.stop();
    volume_fade_.setStartValue(double(audio_->volume()));
    volume_fade_.start();
  } else if (player_ != nullptr) {
    player_->stop();
  }
  frame_ = {};
  frame_blur_ = {};
  trailer_fade_.stop();
}

void HeroBackground::Show(const Item& item) {
  if (item.key == key_) return;
  key_ = item.key;
  StopTrailer();
  trailer_fade_.setDirection(QAbstractAnimation::Forward);
  trailer_fade_.setCurrentTime(0);
  const bool trailers = window_->prefs().big_screen_trailers.value_or(true);
  trailer_key_ = item.game && trailers ? key_ : QString();
  const bool was_showcasing = reveal_;
  reveal_ = false;
  trailer_reveal_.stop();
  const int delay = std::max(0, window_->prefs().big_screen_trailer_delay_ms.value_or(3000));
  if (!key_.isEmpty()) trailer_reveal_.start(delay);
  if (!trailer_key_.isEmpty()) trailer_delay_.start(std::min(delay, kLoadAfterMs));
  if (was_showcasing) emit ShowcaseChanged();

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
  player_->setAudioOutput(audio_);
  player_->setVideoSink(sink_);
  connect(sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
    if (trailer_key_ != key_ || playing_key_ != key_) return;
    // Skips the rating card and logos most trailers open with.
    if (frame.startTime() >= 0 && frame.startTime() < skip_us_) return;
    if (player_->playbackRate() != 1.0) player_->setPlaybackRate(1.0);
    // Loaded early: waits here, paused, until the start delay has passed.
    if (!reveal_) {
      if (player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();
      return;
    }
    const bool first = frame_.isNull();
    if (first && window_->prefs().big_screen_trailer_hd_only.value_or(false) && frame.height() < 720) {
      player_->stop();
      trailer_key_.clear();
      return;
    }
    QImage image = frame.toImage();
    if (image.isNull()) return;
    if (first) {
      audio_->setMuted(!window_->prefs().big_screen_trailer_sound.value_or(false));
      // Blurred from a small copy, so it costs next to nothing.
      frame_blur_ = BlurredCover(QPixmap::fromImage(image.scaled(320, 180, Qt::KeepAspectRatio, Qt::FastTransformation)),
                                 QSize(480, 270));
    }
    frame_ = std::move(image);
    if (first) {
      trailer_fade_.setDirection(QAbstractAnimation::Forward);
      trailer_fade_.start();
    }
    update();
  });
  connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
    if (status == QMediaPlayer::LoadedMedia && player_->duration() <= kSkipFromMs) {
      skip_us_ = 0;
      player_->setPlaybackRate(1.0);
    }
    if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia) EndTrailer();
  });
  connect(player_, &QMediaPlayer::errorOccurred, this, [this] { EndTrailer(); });
  // Trailers play only while the window has focus.
  connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
    const bool active = window_->isActiveWindow();
    if (!active && player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();
    if (active && reveal_ && player_->playbackState() == QMediaPlayer::PausedState) player_->play();
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
  const auto draw = [&](const QPixmap& art, double opacity, double scale = 1.0) {
    if (art.isNull() || opacity <= 0) return;
    painter.setOpacity(opacity);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF to = fill(art.size());
    const QSizeF scaled = to.size() * scale;
    painter.drawPixmap(QRectF(to.center() - QPointF(scaled.width(), scaled.height()) / 2, scaled), art, QRectF(art.rect()));
  };
  const double t = fade_.state() == QAbstractAnimation::Running ? fade_.currentValue().toDouble() : 1.0;
  draw(previous_, 1.0 - t);
  // The new hero settles in from slightly closer as it fades up.
  draw(current_, t, 1.0 + 0.035 * (1.0 - t) * (1.0 - t));
  painter.setOpacity(1.0);

  if (!frame_.isNull()) {
    // In through a blur of its first frame, then the picture sharpens over it.
    const double v = trailer_fade_.currentValue().toDouble();
    draw(frame_blur_, std::min(1.0, v * 2));
    painter.setOpacity(std::clamp(v * 2 - 1, 0.0, 1.0));
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
  // Lower while a trailer shows, so more of it is seen.
  const double k = frame_.isNull() ? 0.0 : std::clamp(trailer_fade_.currentValue().toDouble(), 0.0, 1.0);
  const auto ease = [k](double from, double to) { return from + (to - from) * k; };
  QLinearGradient bottom(0, height(), 0, 0);
  QColor dense = base;
  dense.setAlpha(230);
  bottom.setColorAt(0, solid);
  bottom.setColorAt(ease(0.42, 0.18), dense);
  bottom.setColorAt(ease(0.75, 0.42), clear);
  painter.fillRect(rect(), bottom);
}

}  // namespace mira_gui::bigscreen
