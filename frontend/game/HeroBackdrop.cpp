#include "HeroBackdrop.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QTimer>
#include <algorithm>

#include "../client/api/Artwork.h"
#include "../library/ArtworkStore.h"
#include "../library/CoverArt.h"
#include "../theme/Theme.h"

namespace mira_gui {
namespace {

// Share of the card's height the art covers before it has faded out.
constexpr qreal kBandShare = 0.6;

}  // namespace

HeroBackdrop::HeroBackdrop(ArtworkStore* artwork, QWidget* parent) : QWidget(parent), artwork_(artwork) {
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] { update(); });
}

void HeroBackdrop::ShowGame(const GameSummary& game) {
  game_ = game;
  hero_ = QPixmap();
  rendered_ = QPixmap();
  if (artwork_ != nullptr) artwork_->EnsureRequested(game.id);
  if (!game.art || game.art->contains("hero")) LoadHero();
  update();
}

void HeroBackdrop::RefreshCover(const std::string& id) {
  if (id != game_.id) return;
  cover_preview_ = QPixmap();
  rendered_ = QPixmap();
  update();
}

void HeroBackdrop::SetPreview(const QString& slot, const QPixmap& preview) {
  if (slot != "hero" && slot != "cover") return;  // logos and icons aren't drawn here
  (slot == "hero" ? hero_preview_ : cover_preview_) = preview;
  rendered_ = QPixmap();
  update();
}

void HeroBackdrop::RefreshHero(const std::string& id) {
  if (id != game_.id) return;
  LoadHero();
}

void HeroBackdrop::LoadHero() {
  const std::string id = game_.id;
  // A game with no hero is a 404 here, when its record didn't already say so.
  api::GetArtworkImageAsync(this, id, "hero", [this, id](QImage image) {
    if (game_.id != id || image.isNull()) return;
    hero_ = QPixmap::fromImage(std::move(image));
    hero_preview_ = QPixmap();
    rendered_ = QPixmap();
    update();
  });
}

QPixmap HeroBackdrop::Source() const {
  if (!hero_preview_.isNull()) return hero_preview_;
  if (!hero_.isNull()) return hero_;
  if (!cover_preview_.isNull()) return cover_preview_;
  // Blurred to a wash of its colors, so the small copy is all it needs.
  if (artwork_ != nullptr && artwork_->HasArtwork(game_.id))
    return artwork_->SmallArtwork(game_.id);
  // The same generated art as its tile, so the card still carries its colors.
  return PlaceholderCover(QString::fromStdString(game_.name), QString::fromStdString(game_.id), QSize(200, 300), 1);
}

void HeroBackdrop::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (resize_settled_ == nullptr) {
    resize_settled_ = new QTimer(this);
    resize_settled_->setSingleShot(true);
    resize_settled_->setInterval(150);
    connect(resize_settled_, &QTimer::timeout, this, [this] {
      resizing_ = false;
      rendered_ = QPixmap();
      update();
    });
  }
  resizing_ = true;
  resize_settled_->start();
}

void HeroBackdrop::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  QPainterPath card;
  card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tokens.radius_panel, tokens.radius_panel);
  painter.fillPath(card, tokens.surface);

  const QPixmap source = Source();
  if (!source.isNull()) {
    const QRect band(0, 0, width(), qRound(height() * kBandShare));
    const qreal dpr = devicePixelRatioF();
    const qint64 key = source.cacheKey() ^ (qint64(band.width()) << 32) ^ band.height();
    if (rendered_.isNull() || rendered_key_ != key) {
      // While the card is being resized, a quick scale per step; the smooth one once it stops.
      const Qt::TransformationMode mode =
          resizing_ ? Qt::FastTransformation : Qt::SmoothTransformation;
      QPixmap scaled;
      // A cover is portrait: a slice of it reads as noise, so blur it into
      // a wash of its colors (down to a few pixels and back up).
      if (hero_.isNull() && hero_preview_.isNull()) {
        scaled = BlurredCover(source, band.size() * dpr);
      } else {
        scaled = source.scaled(band.size() * dpr, Qt::KeepAspectRatioByExpanding, mode);
      }
      scaled.setDevicePixelRatio(dpr);
      rendered_ = scaled;
      rendered_key_ = key;
    }
    const QSizeF size = rendered_.deviceIndependentSize();
    painter.save();
    painter.setClipPath(card);
    painter.setClipRect(band, Qt::IntersectClip);
    painter.drawPixmap(QPointF((band.width() - size.width()) / 2, (band.height() - size.height()) / 2),
                       rendered_);
    // Fades into the surface, so text laid over it keeps the theme's colors.
    QLinearGradient fade(0, 0, 0, band.height());
    QColor surface = tokens.surface;
    surface.setAlphaF(0.2);
    fade.setColorAt(0, surface);
    surface.setAlphaF(0.75);
    fade.setColorAt(0.55, surface);
    surface.setAlphaF(1);
    fade.setColorAt(1, surface);
    painter.fillRect(band, fade);
    painter.restore();
  }

  painter.setPen(QPen(tokens.border, 1));
  painter.drawPath(card);
}

CoverChip::CoverChip(ArtworkStore* artwork, QWidget* parent) : QWidget(parent), artwork_(artwork) {
  setFixedSize(48, 72);
  if (artwork_ != nullptr) {
    connect(artwork_, &ArtworkStore::CoverChanged, this,
            [this](const QString& id) { RefreshCover(id.toStdString()); });
  }
}

void CoverChip::ShowGame(const GameSummary& game) {
  game_ = game;
  preview_ = QPixmap();
  preview_filled_ = QPixmap();
  update();
}

void CoverChip::RefreshCover(const std::string& id) {
  if (id != game_.id) return;
  preview_ = QPixmap();
  preview_filled_ = QPixmap();
  update();
}

void CoverChip::SetPreview(const QPixmap& preview) {
  preview_ = preview;
  preview_filled_ = QPixmap();
  update();
}

void CoverChip::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  QPainterPath path;
  path.addRoundedRect(box, tokens.radius_tile, tokens.radius_tile);
  painter.setClipPath(path);
  if (!preview_.isNull()) {
    if (preview_filled_.isNull() || preview_dpr_ != devicePixelRatioF()) {
      preview_filled_ =
          preview_.scaled(size() * devicePixelRatioF(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
      preview_dpr_ = devicePixelRatioF();
    }
    const QPixmap& filled = preview_filled_;
    const QRectF source((filled.width() - width() * devicePixelRatioF()) / 2,
                        (filled.height() - height() * devicePixelRatioF()) / 2, width() * devicePixelRatioF(),
                        height() * devicePixelRatioF());
    painter.drawPixmap(QRectF(rect()), filled, source);
  } else if (artwork_ != nullptr && !game_.id.empty()) {
    painter.drawPixmap(rect(), artwork_->Cover(game_, size(), devicePixelRatioF()));
  }
  painter.setClipping(false);
  painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
  painter.drawPath(path);
}

}  // namespace mira_gui
