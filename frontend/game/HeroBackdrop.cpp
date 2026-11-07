#include "HeroBackdrop.h"

#include <QGraphicsOpacityEffect>
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

// A hero's usual shape (Steam's are 1920x620), for the blurred cover that stands in for one.
constexpr qreal kHeroAspect = 620.0 / 1920.0;
// The least the art covers before it has faded out: the header over it and some of the panel.
constexpr int kMinBand = 240;
// How far a HeroPanel's fill takes to fade in, and its content's top (within the pages' own top
// margins, so nothing at rest fades).
constexpr int kPanelFade = 48;
constexpr int kContentFade = 12;
// How much of the art's color the card takes, on a dark and a light theme.
constexpr qreal kDarkTint = 0.24;
constexpr qreal kLightTint = 0.12;

}  // namespace

HeroBackdrop::HeroBackdrop(ArtworkStore* artwork, QWidget* parent)
    : QWidget(parent), artwork_(artwork) {
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] { update(); });
}

void HeroBackdrop::ShowGame(const GameSummary& game) {
  game_ = game;
  hero_ = QPixmap();
  rendered_ = QPixmap();
  placeholder_ = QPixmap();
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
  if (placeholder_.isNull())
    placeholder_ = PlaceholderCover(QString::fromStdString(game_.name),
                                    QString::fromStdString(game_.id), QSize(200, 300), 1);
  return placeholder_;
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
  card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), tokens.radius_panel,
                      tokens.radius_panel);
  const QPixmap source = Source();
  if (source.cacheKey() != tone_key_) {
    tone_ = source.isNull() ? QColor() : DominantColor(source);
    tone_key_ = source.cacheKey();
  }
  const QColor surface_tinted = Tinted(tokens.surface);
  painter.fillPath(card, surface_tinted);

  if (!source.isNull()) {
    // Across the card's width at the art's own shape, so all of it shows however tall the card.
    const bool hero = !hero_.isNull() || !hero_preview_.isNull();
    const qreal aspect = hero ? qreal(source.height()) / source.width() : kHeroAspect;
    const QRect band(0, 0, width(),
                     std::clamp(qRound(width() * aspect), std::min(kMinBand, height()), height()));
    const qreal dpr = devicePixelRatioF();
    const qint64 key = source.cacheKey() ^ (qint64(band.width()) << 32) ^ band.height() ^
                       (qint64(qRound(dpr * 100)) << 48);
    if (rendered_.isNull() || rendered_key_ != key) {
      // While the card is being resized, a quick scale per step; the smooth one once it stops.
      const Qt::TransformationMode mode =
          resizing_ ? Qt::FastTransformation : Qt::SmoothTransformation;
      QPixmap scaled;
      // A cover is portrait: a slice of it reads as noise, so blur it into
      // a wash of its colors (down to a few pixels and back up).
      if (!hero) {
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
    painter.drawPixmap(
        QPointF((band.width() - size.width()) / 2, (band.height() - size.height()) / 2), rendered_);
    // Fades into the surface, so text laid over it keeps the theme's colors.
    QLinearGradient fade(0, 0, 0, band.height());
    QColor surface = surface_tinted;
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

HeroPanel::HeroPanel(QWidget* parent) : QWidget(parent) {
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] { update(); });
}

QColor HeroBackdrop::Tinted(const QColor& base) const {
  if (!tone_.isValid()) return base;
  // Enough to read as the art's, not so much that the theme's own colors stop showing.
  const qreal amount = base.lightnessF() < 0.5 ? kDarkTint : kLightTint;
  return theme::Mix(base, tone_, amount);
}

void HeroPanel::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  const auto* backdrop = qobject_cast<const HeroBackdrop*>(parentWidget());
  QColor color = backdrop != nullptr ? backdrop->Tinted(tokens.window) : tokens.window;
  color.setAlphaF(0.82);
  QColor clear = color;
  clear.setAlphaF(0);
  QLinearGradient fill(0, 0, 0, height());
  fill.setColorAt(0, clear);
  fill.setColorAt(std::min(1.0, qreal(kPanelFade) / std::max(1, height())), color);
  fill.setColorAt(1, color);
  // The card's corner, less its border.
  const qreal radius = std::max(0, tokens.radius_panel - 1);
  QPainterPath path;
  path.moveTo(0, 0);
  path.lineTo(width(), 0);
  path.lineTo(width(), height() - radius);
  path.arcTo(width() - 2 * radius, height() - 2 * radius, 2 * radius, 2 * radius, 0, -90);
  path.lineTo(radius, height());
  path.arcTo(0, height() - 2 * radius, 2 * radius, 2 * radius, -90, -90);
  path.closeSubpath();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillPath(path, fill);
}

void FadeTop(QWidget* widget) {
  auto* effect = new QGraphicsOpacityEffect(widget);
  effect->setOpacity(1);  // 0.7 by default
  QLinearGradient mask(0, 0, 0, kContentFade);
  mask.setColorAt(0, Qt::transparent);
  mask.setColorAt(1, Qt::black);
  effect->setOpacityMask(mask);
  widget->setGraphicsEffect(effect);
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
      preview_filled_ = preview_.scaled(size() * devicePixelRatioF(),
                                        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
      preview_dpr_ = devicePixelRatioF();
    }
    const QPixmap& filled = preview_filled_;
    const QRectF source((filled.width() - width() * devicePixelRatioF()) / 2,
                        (filled.height() - height() * devicePixelRatioF()) / 2,
                        width() * devicePixelRatioF(), height() * devicePixelRatioF());
    painter.drawPixmap(QRectF(rect()), filled, source);
  } else if (artwork_ != nullptr && !game_.id.empty()) {
    painter.drawPixmap(rect(), artwork_->Cover(game_, size(), devicePixelRatioF()));
  }
  painter.setClipping(false);
  painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
  painter.drawPath(path);
}

}  // namespace mira_gui
