#include "GameTileDelegate.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QFontMetrics>
#include <QTimer>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStandardItem>

#include "ArtworkStore.h"
#include "GamePresentation.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/ProgressRail.h"
#include "../widgets/TileView.h"

namespace mira_gui {
namespace {

constexpr int kScrimHeight = 62;

}  // namespace

QRect GameTileDelegate::ActionRect(const QRect& cell, const QString& text, const QFont& font) {
  const int inset = theme::Current().tile_spacing;
  QFont bold = font;
  bold.setWeight(QFont::DemiBold);
  const int width = QFontMetrics(bold).horizontalAdvance(text) + 20;
  return QRect(cell.right() - inset - 6 - width + 1, cell.top() + inset + 6, width, 24);
}

void GameTileDelegate::SetTileProgress(QStandardItem& item,
                                       const std::optional<DownloadTracker::TileProgress>& installing,
                                       const QString& idle_action) {
  item.setData(installing ? QString() : idle_action, ActionRole);
  item.setData(!installing, ActionEnabledRole);
  item.setData(installing ? QVariant(installing->fraction) : QVariant(), ProgressRole);
  item.setData(installing ? QVariant(installing->status) : QVariant(), StatusTextRole);
  item.setData(installing ? QVariant(installing->detail) : QVariant(), ProgressDetailRole);
}

GameTileDelegate::GameTileDelegate(QObject* parent, QSize tile, ArtworkStore* artwork)
    : QStyledItemDelegate(parent), tile_(tile), artwork_(artwork) {}

void GameTileDelegate::SetTileSize(QSize tile) { tile_ = tile; }

void GameTileDelegate::ShowNote(QAbstractItemView* view, const QString& id, const QString& text) {
  auto* delegate = dynamic_cast<GameTileDelegate*>(view->itemDelegate());
  if (delegate == nullptr) return;
  delegate->note_id_ = id;
  delegate->note_ = text;
  view->viewport()->update();
  QTimer::singleShot(3000, view, [view, delegate, id] {
    if (delegate->note_id_ != id) return;  // a newer note replaced it
    delegate->note_.clear();
    delegate->note_id_.clear();
    view->viewport()->update();
  });
}

QSize GameTileDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
  return tile_;
}

void GameTileDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const {
  const theme::Tokens& tokens = theme::Current();
  const bool selected = option.state & QStyle::State_Selected;
  const bool hovered = option.state & QStyle::State_MouseOver;

  // The gap between tiles is this inset, not QListView::spacing: the grid
  // cell stays the size the zoom slider asked for either way. On hover, the
  // tile grows into part of that gap -- never past it, so it can't touch a
  // neighbor -- and what's left of the gap becomes room for a soft shadow
  // (below), grown outward from the un-hovered inset so neither ever
  // crosses into the next cell.
  const int inset = tokens.tile_spacing;
  const int grow = hovered ? qMin(inset, 2) : 0;
  const QRect rect = option.rect.adjusted(inset - grow, inset - grow, -(inset - grow), -(inset - grow));
  if (rect.isEmpty()) return;

  const QString name = index.data(NameRole).toString();
  const std::string status = index.data(StatusRole).toString().toStdString();
  const bool running = index.data(RunningRole).toBool();

  QPainterPath path;
  if (tokens.radius_tile > 0) {
    path.addRoundedRect(rect, tokens.radius_tile, tokens.radius_tile);
  } else {
    path.addRect(rect);
  }

  if (hovered) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    // Concentric, decreasingly transparent fills: each smaller one paints
    // over the middle of the last, leaving only its own outer ring visible
    // -- the usual cheap stand-in for a real blur. Capped to what's left of
    // the gap after grow above claimed its share, so it can't reach the
    // neighbor either.
    const int shadow_budget = qMax(1, inset - grow);
    for (int i = shadow_budget; i >= 1; --i) {
      painter->setBrush(QColor(0, 0, 0, 8 + (shadow_budget - i) * 6));
      const QRect ring = rect.adjusted(-i, -i, i, i);
      if (tokens.radius_tile > 0) {
        painter->drawRoundedRect(ring, tokens.radius_tile + i, tokens.radius_tile + i);
      } else {
        painter->drawRect(ring);
      }
    }
    painter->restore();
  }

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);
  painter->setClipPath(path);

  QPixmap cover = index.data(Qt::DecorationRole).value<QPixmap>();
  if (cover.isNull() && artwork_ != nullptr) {
    const qreal dpr = painter->device() != nullptr ? painter->device()->devicePixelRatioF() : 1.0;
    cover = artwork_->CoverById(index.data(IdRole).toString(), name, tile_, dpr);
  }
  if (cover.isNull()) {
    painter->fillRect(rect, tokens.tile_placeholder);
  } else {
    painter->drawPixmap(rect, cover);
  }

  // The scrim keeps a light cover from swallowing the title, drawn
  // regardless of the artwork underneath. +1 on the top edge:
  // QRect::bottom() is the last pixel, so without it the scrim fell one
  // pixel short of the tile's actual bottom edge.
  const QVariant progress = index.data(ProgressRole);
  // Taller under an install's two lines.
  const int scrim_height = qMin(rect.height(), progress.isValid() ? kScrimHeight + 28 : kScrimHeight);
  const QRect scrim(rect.left(), rect.bottom() - scrim_height + 1, rect.width(), scrim_height);
  QLinearGradient gradient(scrim.bottomLeft(), scrim.topLeft());
  gradient.setColorAt(0.0, QColor(0, 0, 0, tokens.scrim_alpha));
  gradient.setColorAt(1.0, QColor(0, 0, 0, 0));
  painter->fillRect(scrim, gradient);

  // Between the title and the status line, which says how far along it is.
  if (progress.isValid()) {
    PaintRail(*painter, QRectF(rect.left() + 8, rect.bottom() - 40, rect.width() - 16, 4), progress.toDouble(),
              tokens.accent, QColor(255, 255, 255, 40));
    if (progress.toDouble() < 0) {
      if (auto* view = dynamic_cast<TileView*>(const_cast<QWidget*>(option.widget))) view->KeepAnimating();
    }
  }
  painter->restore();

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);

  QFont title_font = option.font;
  title_font.setWeight(QFont::DemiBold);
  painter->setFont(title_font);
  painter->setPen(QColor(255, 255, 255, 235));
  const QRect title_rect(rect.left() + 8, rect.bottom() - (progress.isValid() ? 60 : 36), rect.width() - 16, 18);
  painter->drawText(title_rect, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(title_font).elidedText(name, Qt::ElideRight, title_rect.width()));

  QFont small_font = option.font;
  small_font.setPixelSize(qMax(9, small_font.pixelSize() > 0 ? small_font.pixelSize() - 2 : 10));
  // An install: how far along under the rail, and its speed or size under that.
  if (progress.isValid()) {
    painter->setFont(small_font);
    const QFontMetrics metrics(small_font);
    const QRect status_rect(rect.left() + 8, rect.bottom() - 34, rect.width() - 16, 15);
    painter->setPen(QColor(255, 255, 255, 235));
    painter->drawText(status_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      metrics.elidedText(index.data(StatusTextRole).toString(), Qt::ElideRight, status_rect.width()));
    const QRect detail_rect(rect.left() + 8, rect.bottom() - 19, rect.width() - 16, 15);
    painter->setPen(QColor(255, 255, 255, 160));
    painter->drawText(detail_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      metrics.elidedText(index.data(ProgressDetailRole).toString(), Qt::ElideRight, detail_rect.width()));
  }

  // "Ready" says nothing worth a line on every tile, only a state that
  // needs attention (or Playing) earns one.
  const QString status_text = index.data(StatusTextRole).toString();
  // Only when nothing more pressing is on the line.
  const bool unchecked = !running && status == "ready" && status_text.isEmpty() && index.data(NeedsCheckRole).toBool();
  if (!progress.isValid() && show_status_ && (running || status != "ready" || !status_text.isEmpty() || unchecked)) {
    painter->setFont(small_font);
    const QRect status_rect(rect.left() + 8, rect.bottom() - 19, rect.width() - 16, 15);
    QColor dot = status_text.isEmpty() ? StatusColor(status) : tokens.status_setting_up;
    if (unchecked) dot = tokens.warning;
    painter->setPen(Qt::NoPen);
    painter->setBrush(dot.lighter(160));
    painter->drawEllipse(QPoint(status_rect.left() + 3, status_rect.center().y()), 3, 3);
    painter->setPen(QColor(255, 255, 255, 170));
    painter->drawText(status_rect.adjusted(12, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                      !status_text.isEmpty() ? status_text
                      : running              ? QString("Playing")
                      : unchecked            ? QString("Not checked")
                                             : StatusLabel(status));
  }

  if (show_source_mark_) {
    if (const SourceInfo* source = FindSourceInfo(index.data(SourceRole).toString())) {
      const QRect mark(rect.left() + 6, rect.top() + 6, 20, 20);
      painter->setPen(Qt::NoPen);
      painter->setBrush(source->color);
      painter->drawRoundedRect(mark, 5, 5);
      QFont mark_font = option.font;
      mark_font.setWeight(QFont::Bold);
      mark_font.setPixelSize(11);
      painter->setFont(mark_font);
      painter->setPen(Qt::white);
      painter->drawText(mark, Qt::AlignCenter, source->name.left(1));
    }
  }

  const QString action = index.data(ActionRole).toString();
  QRect pill;
  if (!action.isEmpty()) {
    const bool enabled = index.data(ActionEnabledRole).toBool();
    pill = ActionRect(option.rect, action, option.font);
    QFont pill_font = option.font;
    pill_font.setWeight(QFont::DemiBold);
    painter->setFont(pill_font);
    painter->setPen(Qt::NoPen);
    painter->setBrush(enabled ? tokens.accent : QColor(0, 0, 0, 150));
    painter->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
    painter->setPen(enabled ? tokens.on_accent : QColor(255, 255, 255, 200));
    painter->drawText(pill, Qt::AlignCenter, action);
  }

  // Top-right, opposite the source mark; left of the ActionRole pill if there is one.
  if (show_pin_badge_ && index.data(PinnedRole).toBool()) {
    const int right = pill.isNull() ? rect.right() - 6 : pill.left() - 4;
    const QRect badge(right - 22 + 1, rect.top() + 6, 22, 22);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 150));
    painter->drawEllipse(badge);
    static const QIcon kPin = icons::For(icons::Glyph::Pin, QColor(255, 255, 255, 235));
    kPin.paint(painter, badge.adjusted(4, 4, -4, -4));
  }

  if (!note_.isEmpty() && index.data(IdRole).toString() == note_id_) {
    QFont note_font = option.font;
    note_font.setWeight(QFont::DemiBold);
    painter->setFont(note_font);
    const QFontMetrics metrics(note_font);
    // Wraps on a narrow tile rather than cutting the words off.
    const QRect text = metrics.boundingRect(QRect(0, 0, rect.width() - 32, rect.height()),
                                            Qt::AlignCenter | Qt::TextWordWrap, note_);
    const QRect pill(rect.center().x() - (text.width() + 20) / 2, rect.center().y() - (text.height() + 12) / 2,
                     text.width() + 20, text.height() + 12);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 200));
    painter->drawRoundedRect(pill, 12, 12);
    painter->setPen(QColor(255, 255, 255, 240));
    painter->drawText(pill, Qt::AlignCenter | Qt::TextWordWrap, note_);
  }

  // Border last, so selection reads on top of the artwork.
  painter->setBrush(Qt::NoBrush);
  if (running) {
    painter->setPen(QPen(tokens.running, 2));
    painter->drawPath(path);
  }
  if (selected) {
    painter->setPen(QPen(tokens.accent, 3));
    painter->drawPath(path);
  } else if (hovered) {
    painter->setPen(QPen(QColor(255, 255, 255, 80), 2));
    painter->drawPath(path);
  }
  painter->restore();
}

}  // namespace mira_gui
