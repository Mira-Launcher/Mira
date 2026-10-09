#include "HeroBackground.h"

#include <QLinearGradient>
#include <QPainter>

#include "../client/api/Artwork.h"
#include "../library/CoverArt.h"
#include "../theme/Theme.h"
#include "BigScreenWindow.h"

namespace mira_gui::bigscreen {

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
}

void HeroBackground::Show(const Item& item) {
  if (item.key == key_) return;
  key_ = item.key;
  const auto hero = heroes_.constFind(key_);
  if (hero != heroes_.constEnd() && !hero->isNull()) return Fade(*hero);
  Fade(Blurred(item));
  if (hero == heroes_.constEnd()) load_.start();
}

void HeroBackground::Fade(const QPixmap& art) {
  previous_ = current_;
  current_ = art;
  fade_.stop();
  fade_.start();
}

void HeroBackground::LoadHero() {
  const QString key = key_;
  // ArtworkStore keeps only a small copy of most heroes, too coarse for a whole screen.
  api::GetArtworkImageAsync(this, key.toStdString(), "hero", [this, key](QImage image) {
    if (heroes_.size() > 16) heroes_.clear();
    const QPixmap hero = image.isNull() ? QPixmap() : QPixmap::fromImage(std::move(image));
    heroes_.insert(key, hero);
    if (key == key_ && !hero.isNull()) Fade(hero);
  });
}

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
  const auto draw = [&](const QPixmap& art, double opacity) {
    if (art.isNull() || opacity <= 0) return;
    // Fill the screen, cropping the longer side around the center.
    const QSizeF scaled = QSizeF(art.size()).scaled(size(), Qt::KeepAspectRatioByExpanding);
    const QRectF target((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled.width(),
                        scaled.height());
    painter.setOpacity(opacity);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(target, art, QRectF(art.rect()));
  };
  const double t = fade_.state() == QAbstractAnimation::Running ? fade_.currentValue().toDouble() : 1.0;
  draw(previous_, 1.0 - t);
  draw(current_, t);
  painter.setOpacity(1.0);

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
