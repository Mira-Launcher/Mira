#include "SidebarGames.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

#include "../library/ArtworkStore.h"
#include "../library/CoverArt.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"

namespace mira_gui::sidebar {
namespace {

const QSize kThumb(20, 30);
constexpr int kHeroHeight = 40;

// `source` scaled to cover `size` and centre-cropped, with rounded corners.
QPixmap RoundedCrop(const QPixmap& source, QSize size, qreal radius, qreal dpr) {
  QPixmap out(size * dpr);
  out.setDevicePixelRatio(dpr);
  out.fill(Qt::transparent);
  QPainter painter(&out);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(size)), radius, radius);
  painter.setClipPath(clip);
  const QSizeF filled = QSizeF(source.size()).scaled(QSizeF(size), Qt::KeepAspectRatioByExpanding);
  painter.drawPixmap(QRectF(QPointF((size.width() - filled.width()) / 2, (size.height() - filled.height()) / 2), filled),
                     source, QRectF(source.rect()));
  return out;
}

QPixmap CoverThumb(const GameSummary& game, ArtworkStore* artwork, qreal dpr) {
  const QPixmap art = artwork->SmallArtwork(game.id);
  if (!art.isNull()) return RoundedCrop(art, kThumb, 3, dpr);
  // Not fetched yet, or none: the placeholder's color.
  artwork->EnsureRequested(game.id);
  QPixmap out(kThumb * dpr);
  out.setDevicePixelRatio(dpr);
  out.fill(Qt::transparent);
  QPainter painter(&out);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(PlaceholderBase(QString::fromStdString(game.id)));
  painter.drawRoundedRect(QRectF(QPointF(0, 0), QSizeF(kThumb)), 3, 3);
  return out;
}

// A shelf cover's "Yesterday" or "Playing", over a dark fade at its foot.
void DrawCoverLabel(QPainter* painter, const QRectF& cover, const QString& text, int pixel_size) {
  if (text.isEmpty()) return;
  painter->save();
  const QRectF foot(cover.left(), cover.bottom() - cover.height() * 0.4, cover.width(), cover.height() * 0.4);
  QLinearGradient fade(foot.topLeft(), foot.bottomLeft());
  fade.setColorAt(0.0, QColor(0, 0, 0, 0));
  fade.setColorAt(1.0, QColor(0, 0, 0, 210));
  painter->fillRect(foot, fade);
  QFont font = painter->font();
  font.setPixelSize(pixel_size);
  font.setWeight(QFont::DemiBold);
  painter->setFont(font);
  // White even for "Playing": green on bright art is unreadable, and the cover's ring already says so.
  painter->setPen(QColor(255, 255, 255, 235));
  const QRectF line = foot.adjusted(3, 0, -3, -3);
  painter->drawText(line, Qt::AlignHCenter | Qt::AlignBottom,
                    QFontMetrics(font).elidedText(text, Qt::ElideRight, qRound(line.width())));
  painter->restore();
}

QString Rgba(QColor color, double alpha) {
  color.setAlphaF(alpha);
  return theme::ColorToQss(color);
}

}  // namespace

const std::vector<StyleOption>& StyleOptions() {
  static const std::vector<StyleOption> kOptions = {
      {Style::Covers, "covers", "Covers"},
      {Style::Hero, "hero", "Banners"},
      {Style::Shelf, "shelf", "Shelf"},
  };
  return kOptions;
}

Style ParseStyle(const std::string& key) {
  for (const StyleOption& option : StyleOptions()) {
    if (key == option.key) return option.style;
  }
  return Style::Covers;
}

const char* StyleKey(Style style) {
  for (const StyleOption& option : StyleOptions()) {
    if (option.style == style) return option.key;
  }
  return "covers";
}

namespace {

// `art` cropped to fill `target`, corners rounded; the placeholder's color without art.
void DrawArt(QPainter* painter, const QRectF& target, const QPixmap& art, const std::string& id, qreal radius) {
  painter->save();
  QPainterPath clip;
  clip.addRoundedRect(target, radius, radius);
  painter->setClipPath(clip);
  if (art.isNull()) {
    painter->fillRect(target, PlaceholderBase(QString::fromStdString(id)));
  } else {
    const QSizeF shown = target.size().scaled(QSizeF(art.size()), Qt::KeepAspectRatio);
    const QRectF source(QPointF((art.width() - shown.width()) / 2, (art.height() - shown.height()) / 2), shown);
    painter->drawPixmap(target, art, source);
  }
  painter->restore();
}

QPixmap CoverArtOf(const std::string& id, ArtworkStore* artwork) {
  const QPixmap art = artwork->RawArtwork(id);
  if (art.isNull()) artwork->EnsureRequested(id);
  return art;
}

void PaintRealPreview(QPainter* painter, const QRect& rect, Style style, const std::vector<PreviewGame>& games,
                      ArtworkStore* artwork) {
  const theme::Tokens& tokens = theme::Current();
  QFont name_font = painter->font();
  name_font.setPixelSize(11);
  QFont trailing_font = painter->font();
  trailing_font.setPixelSize(9);
  const int rows = std::min<int>(3, static_cast<int>(games.size()));
  const int row = rect.height() / 3;
  switch (style) {
    case Style::Covers:
      for (int i = 0; i < rows; ++i) {
        const PreviewGame& game = games[i];
        const QRect line(rect.left(), rect.top() + i * row, rect.width(), row);
        const int thumb_height = row - 4;
        DrawArt(painter, QRectF(line.left() + 2, line.top() + 2, thumb_height * 2 / 3, thumb_height),
                CoverArtOf(game.id, artwork), game.id, 2);
        QRect text = line.adjusted(thumb_height * 2 / 3 + 8, 0, -2, 0);
        if (!game.trailing.isEmpty()) {
          painter->setFont(trailing_font);
          painter->setPen(game.trailing == "Playing" ? tokens.running : tokens.text_muted);
          painter->drawText(text, Qt::AlignRight | Qt::AlignVCenter, game.trailing);
          text.setRight(text.right() - QFontMetrics(trailing_font).horizontalAdvance(game.trailing) - 5);
        }
        painter->setFont(name_font);
        painter->setPen(tokens.text);
        painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(name_font).elidedText(game.name, Qt::ElideRight, text.width()));
      }
      break;
    case Style::Hero:
      name_font.setWeight(QFont::DemiBold);
      for (int i = 0; i < rows; ++i) {
        const PreviewGame& game = games[i];
        const QRectF banner(rect.left(), rect.top() + i * row + 1, rect.width(), row - 3);
        QPixmap art = artwork->SlotArt(game.id, "hero");
        if (art.isNull()) art = CoverArtOf(game.id, artwork);
        DrawArt(painter, banner, art, game.id, 3);
        QLinearGradient scrim(banner.topLeft(), banner.topRight());
        scrim.setColorAt(0.0, QColor(0, 0, 0, 185));
        scrim.setColorAt(0.6, QColor(0, 0, 0, 105));
        scrim.setColorAt(1.0, QColor(0, 0, 0, game.trailing.isEmpty() ? 30 : 165));
        QPainterPath clip;
        clip.addRoundedRect(banner, 3, 3);
        painter->fillPath(clip, scrim);
        QRect text = banner.toRect().adjusted(6, 0, -5, 0);
        if (!game.trailing.isEmpty()) {
          painter->setFont(trailing_font);
          painter->setPen(game.trailing == "Playing" ? tokens.running : QColor(255, 255, 255, 215));
          painter->drawText(text, Qt::AlignRight | Qt::AlignVCenter, game.trailing);
          text.setRight(text.right() - QFontMetrics(trailing_font).horizontalAdvance(game.trailing) - 5);
        }
        painter->setFont(name_font);
        painter->setPen(QColor(255, 255, 255, 240));
        painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(name_font).elidedText(game.name, Qt::ElideRight, text.width()));
      }
      break;
    case Style::Shelf: {
      // The first row, at the size the sidebar would draw it, shrunk to fit if tall.
      const int columns = ShelfColumns(static_cast<int>(games.size()));
      const int gap = 5;
      qreal width = (rect.width() - (columns - 1) * gap) / qreal(columns);
      qreal height = width * 1.5;
      if (height > rect.height()) {
        height = rect.height();
        width = height / 1.5;
      }
      const int shown = std::min<int>(columns, static_cast<int>(games.size()));
      const qreal left = rect.left() + (rect.width() - (shown * width + (shown - 1) * gap)) / 2;
      const qreal top = rect.top() + (rect.height() - height) / 2;
      for (int i = 0; i < shown; ++i) {
        const QRectF cover(left + i * (width + gap), top, width, height);
        DrawArt(painter, cover, CoverArtOf(games[i].id, artwork), games[i].id, 3);
        painter->save();
        QPainterPath clip;
        clip.addRoundedRect(cover, 3, 3);
        painter->setClipPath(clip);
        DrawCoverLabel(painter, cover, games[i].short_trailing, 8);
        painter->restore();
      }
      break;
    }
  }
}

}  // namespace

void PaintStylePreview(QPainter* painter, const QRect& rect, Style style, const std::vector<PreviewGame>& games,
                       ArtworkStore* artwork) {
  if (!games.empty() && artwork != nullptr) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    PaintRealPreview(painter, rect, style, games, artwork);
    painter->restore();
    return;
  }
  // Nothing in the section yet: a sketch of the style instead.
  const theme::Tokens& tokens = theme::Current();
  const std::array<QColor, 4> art = theme::SampleArt(tokens);
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);
  painter->setPen(Qt::NoPen);
  const QColor line = tokens.text_muted;
  switch (style) {
    case Style::Covers: {
      const int row = rect.height() / 3;
      for (int i = 0; i < 3; ++i) {
        const int y = rect.top() + i * row;
        painter->setBrush(art[i]);
        painter->drawRoundedRect(QRectF(rect.left() + 4, y + 2, row * 2 / 3 - 1, row - 4), 2, 2);
        painter->setBrush(line);
        painter->drawRoundedRect(QRectF(rect.left() + 4 + row * 2 / 3 + 6, y + row / 2 - 2, rect.width() * (0.55 - 0.08 * i), 4),
                                 2, 2);
      }
      break;
    }
    case Style::Hero: {
      const int row = rect.height() / 3;
      for (int i = 0; i < 3; ++i) {
        const QRectF banner(rect.left(), rect.top() + i * row + 1, rect.width(), row - 3);
        QLinearGradient fill(banner.topLeft(), banner.topRight());
        fill.setColorAt(0.0, art[i].darker(220));
        fill.setColorAt(1.0, art[i]);
        painter->setBrush(fill);
        painter->drawRoundedRect(banner, 3, 3);
        painter->setBrush(QColor(255, 255, 255, 220));
        painter->drawRoundedRect(QRectF(banner.left() + 7, banner.center().y() - 2, banner.width() * (0.42 - 0.06 * i), 4),
                                 2, 2);
      }
      break;
    }
    case Style::Shelf: {
      const int gap = 5;
      const qreal width = (rect.width() - 3 * gap) / 4.0;
      const qreal height = std::min<qreal>(width * 1.5, rect.height());
      for (int i = 0; i < 4; ++i) {
        painter->setBrush(art[i]);
        painter->drawRoundedRect(
            QRectF(rect.left() + i * (width + gap), rect.top() + (rect.height() - height) / 2, width, height), 3, 3);
      }
      break;
    }
  }
  painter->restore();
}

QPushButton* MakeCoverRow(const GameSummary& game, ArtworkStore* artwork, const QString& trailing, QWidget* parent) {
  // The button only draws the background; the cover, name and trailing text
  // are labels, so a long name elides short of the trailing text.
  const QString name = QString::fromStdString(game.name);
  auto* row = new QPushButton(parent);
  row->setFlat(true);
  row->setAccessibleName(name);
  row->setMinimumHeight(kThumb.height() + 8);
  // Only hover and press take the cover's color; at rest the rows stay plain.
  const QColor color = artwork->CoverColor(QString::fromStdString(game.id));
  row->setStyleSheet(QString("QPushButton:hover { background: %1; } QPushButton:pressed { background: %2; } "
                             "QPushButton[selected=\"true\"] { background: %3; }")
                         .arg(Rgba(color, 0.3), Rgba(color, 0.45), Rgba(theme::Current().accent, 0.3)));
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(8, 0, 10, 0);
  layout->setSpacing(10);
  auto* thumb = new QLabel(row);
  thumb->setFixedSize(kThumb);
  thumb->setPixmap(CoverThumb(game, artwork, row->devicePixelRatioF()));
  layout->addWidget(thumb);
  auto* name_label = new ElidedLabel(name, row);
  name_label->setMinimumWidth(20);
  name_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  layout->addWidget(name_label, /*stretch=*/1);
  if (!trailing.isEmpty()) {
    auto* label = new QLabel(trailing, row);
    label->setProperty("role", "muted");
    // Small, so the name keeps as much of a narrow sidebar as it can.
    QString style = QString("font-size: %1px;").arg(theme::Current().font_size_small);
    if (game.running) style += QString(" color: %1;").arg(theme::Current().running.name());
    label->setStyleSheet(style);
    layout->addWidget(label);
  }
  for (QLabel* label : row->findChildren<QLabel*>()) label->setAttribute(Qt::WA_TransparentForMouseEvents);
  return row;
}

HeroRow::HeroRow(const GameSummary& game, ArtworkStore* artwork, const QString& trailing, QWidget* parent)
    : QPushButton(QString::fromStdString(game.name), parent),
      id_(game.id),
      running_(game.running),
      trailing_(trailing),
      artwork_(artwork) {
  setFlat(true);
  setMinimumHeight(kHeroHeight);
  setAttribute(Qt::WA_Hover);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QSize HeroRow::sizeHint() const { return QSize(QPushButton::sizeHint().width(), kHeroHeight); }

void HeroRow::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  const qreal dpr = devicePixelRatioF();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(rect()), tokens.radius_control, tokens.radius_control);
  painter.setClipPath(clip);

  QPixmap art = artwork_->SlotArt(id_, "hero");
  if (art.isNull()) art = artwork_->RawArtwork(id_);  // no hero: the cover, cropped wide
  if (art.isNull()) {
    artwork_->EnsureRequested(id_);
    painter.fillRect(rect(), PlaceholderBase(QString::fromStdString(id_)));
  } else {
    if (scaled_.size() != size() * dpr) scaled_ = RoundedCrop(art, size(), 0, dpr);
    painter.drawPixmap(0, 0, scaled_);
  }

  // Dark in every theme, like a tile's title scrim: a light one washes the art out.
  // Heaviest behind the name, so it reads over any art; lighter on hover.
  const bool hovered = underMouse();
  QLinearGradient scrim(0, 0, width(), 0);
  scrim.setColorAt(0.0, QColor(0, 0, 0, hovered ? 150 : 185));
  scrim.setColorAt(0.6, QColor(0, 0, 0, hovered ? 70 : 105));
  // Dark at the right too when there's trailing text, so it reads over bright art.
  const int right = trailing_.isEmpty() ? 30 : 165;
  scrim.setColorAt(1.0, QColor(0, 0, 0, hovered ? right - 30 : right));
  painter.fillRect(rect(), scrim);
  if (isDown()) painter.fillRect(rect(), QColor(0, 0, 0, 60));

  QFont font = this->font();
  QRect text_rect = rect().adjusted(12, 0, -10, 0);
  if (!trailing_.isEmpty()) {
    QFont small = font;
    small.setPixelSize(tokens.font_size_small);
    painter.setFont(small);
    painter.setPen(running_ ? tokens.running : QColor(255, 255, 255, 215));
    painter.drawText(text_rect, Qt::AlignRight | Qt::AlignVCenter, trailing_);
    text_rect.setRight(text_rect.right() - QFontMetrics(small).horizontalAdvance(trailing_) - 8);
  }
  font.setWeight(QFont::DemiBold);
  painter.setFont(font);
  painter.setPen(QColor(255, 255, 255, 240));
  painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(font).elidedText(text(), Qt::ElideRight, text_rect.width()));
  // Selected in the library grid too: the grid's own accent outline.
  const bool selected = property("selected").toBool();
  if (running_ || selected) {
    painter.setClipping(false);
    painter.setPen(QPen(selected ? tokens.accent : tokens.running, 2));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), tokens.radius_control, tokens.radius_control);
  }
}

ShelfCover::ShelfCover(const GameSummary& game, ArtworkStore* artwork, const QString& trailing, QWidget* parent)
    : QPushButton(parent),
      id_(QString::fromStdString(game.id)),
      name_(QString::fromStdString(game.name)),
      running_(game.running),
      trailing_(trailing),
      artwork_(artwork) {
  setFlat(true);
  setAttribute(Qt::WA_Hover);
  setAccessibleName(name_);
}

void ShelfCover::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const qreal radius = std::min(tokens.radius_tile, 4);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(rect()), radius, radius);
  painter.setClipPath(clip);
  painter.drawPixmap(rect(), artwork_->CoverById(id_, name_, size(), devicePixelRatioF()));
  DrawCoverLabel(&painter, QRectF(rect()), trailing_, tokens.font_size_small - 1);
  painter.setClipping(false);
  const bool selected = property("selected").toBool();
  if (running_ || selected || underMouse()) {
    const QColor edge = selected ? tokens.accent : running_ ? tokens.running : QColor(255, 255, 255, 110);
    painter.setPen(QPen(edge, 2));
    painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), radius, radius);
  }
}

Shelf::Shelf(QWidget* parent) : QWidget(parent) {
  QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  policy.setHeightForWidth(true);
  setSizePolicy(policy);
}

PlaceholderRow::PlaceholderRow(Style style, int index, QWidget* parent)
    : QWidget(parent), style_(style), index_(index) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QSize PlaceholderRow::sizeHint() const {
  return QSize(200, style_ == Style::Hero ? kHeroHeight : kThumb.height() + 8);
}

void PlaceholderRow::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  const QColor art = theme::SampleArt(tokens)[index_ % 4];
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setOpacity(0.35);
  // Name bars shorten down the list, as in the sketch.
  const qreal bar = 0.55 - 0.08 * (index_ % 4);
  switch (style_) {
    case Style::Covers: {
      // Where MakeCoverRow puts its thumbnail and name.
      const QRectF thumb(8, (height() - kThumb.height()) / 2.0, kThumb.width(), kThumb.height());
      painter.setBrush(art);
      painter.drawRoundedRect(thumb, 3, 3);
      painter.setBrush(tokens.text_muted);
      painter.drawRoundedRect(
          QRectF(thumb.right() + 10, height() / 2.0 - 3, (width() - thumb.right() - 20) * bar, 6),
          3, 3);
      break;
    }
    case Style::Hero: {
      QLinearGradient fill(0, 0, width(), 0);
      fill.setColorAt(0.0, art.darker(220));
      fill.setColorAt(1.0, art);
      painter.setBrush(fill);
      painter.drawRoundedRect(QRectF(rect()), tokens.radius_control, tokens.radius_control);
      painter.setBrush(QColor(255, 255, 255, 220));
      painter.drawRoundedRect(QRectF(12, height() / 2.0 - 3, width() * (bar - 0.13), 6), 3, 3);
      break;
    }
    case Style::Shelf:
      painter.setBrush(art);
      painter.drawRoundedRect(QRectF(rect()), std::min(tokens.radius_tile, 4),
                              std::min(tokens.radius_tile, 4));
      break;
  }
}

void Shelf::Add(QWidget* cover) {
  cover->setParent(this);
  covers_.push_back(cover);
  updateGeometry();
}

int ShelfColumns(int count) { return std::clamp(count, 2, 4); }

int Shelf::Columns() const { return ShelfColumns(static_cast<int>(covers_.size())); }

int Shelf::CellWidth(int width) const { return std::max(1, (width - (Columns() - 1) * kGap) / Columns()); }

int Shelf::heightForWidth(int width) const {
  const int rows = (static_cast<int>(covers_.size()) + Columns() - 1) / Columns();
  if (rows == 0) return 0;
  return rows * (CellWidth(width) * 3 / 2) + (rows - 1) * kGap + 4;
}

QSize Shelf::sizeHint() const { return QSize(200, heightForWidth(width() > 0 ? width() : 200)); }

void Shelf::resizeEvent(QResizeEvent*) {
  const int cell = CellWidth(width());
  for (int i = 0; i < static_cast<int>(covers_.size()); ++i) {
    const int column = i % Columns();
    const int row = i / Columns();
    covers_[i]->setGeometry(column * (cell + kGap), 2 + row * (cell * 3 / 2 + kGap), cell, cell * 3 / 2);
  }
}

QString ArtSignature(const GameSummary& game, Style style, ArtworkStore* artwork) {
  const bool cover = artwork->HasArtwork(game.id);
  switch (style) {
    case Style::Covers:
      return QString("%1%2").arg(artwork->CoverColor(QString::fromStdString(game.id)).name()).arg(cover);
    case Style::Hero: return QString("%1%2").arg(!artwork->SlotArt(game.id, "hero").isNull()).arg(cover);
    case Style::Shelf: return QString::number(cover);
  }
  return QString();
}

}  // namespace mira_gui::sidebar
