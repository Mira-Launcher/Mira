#include "Chip.h"

#include <QEvent>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>

#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "Popover.h"
#include "PopupCard.h"
#include "Variants.h"
#include "paint/Pill.h"

namespace mira_gui {
namespace {

// At the tuned text size, scaled with it.
constexpr int kPaddingX = 10;
constexpr int kPaddingY = 5;
constexpr int kGap = 6;

}  // namespace

Chip::Chip(const QString& text, QWidget* parent) : QAbstractButton(parent) {
  setText(text);
  setAttribute(Qt::WA_Hover);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    updateGeometry();
    update();
  });
  connect(this, &QAbstractButton::clicked, this, [this] {
    if (menu_ != nullptr) {
      menu_->popup(popup::PlaceBelow(this, menu_->sizeHint(), popup::Align::Start, Popover::kGap));
    }
  });
}

void Chip::SetCount(int count) {
  if (count == count_) return;
  count_ = count;
  updateGeometry();
  update();
}

QMenu* Chip::Menu() const {
  return menu_;
}

void Chip::SetMenu(QMenu* menu) {
  menu_ = menu;
  updateGeometry();
  update();
}

void Chip::SetRemovable(bool removable, const QString& tip) {
  removable_ = removable;
  remove_tip_ = tip;
  // The × has no focus of its own, so assistive tools hear how to use it from the chip.
  setAccessibleDescription(removable && !tip.isEmpty() ? tip + " (Delete)" : QString());
  updateGeometry();
  update();
}

void Chip::SetMarked(bool marked) {
  marked_ = marked;
  update();
}

void Chip::SetTint(const QColor& tint) {
  tint_ = tint;
  update();
}

int Chip::TrailingSize() const {
  return menu_ != nullptr || removable_ ? fontMetrics().ascent() : 0;
}

Chip::Layout Chip::LayOut() const {
  const QFontMetrics metrics = fontMetrics();
  const int pad = theme::TextPx(kPaddingX), gap = theme::TextPx(kGap);
  const int icon_px = icon().isNull() ? 0 : std::min(iconSize().height(), metrics.height());
  Layout at;
  qreal x = pad;
  const qreal mid = height() / 2.0;
  if (icon_px > 0) {
    at.icon = QRectF(x, mid - icon_px / 2.0, icon_px, icon_px);
    x += icon_px + gap;
  }
  at.text = QRectF(x, 0, metrics.horizontalAdvance(text()), height());
  x = at.text.right();
  if (count_ >= 0) {
    x += gap;
    at.count = QRectF(x, 0, metrics.horizontalAdvance(QString::number(count_)), height());
    x = at.count.right();
  }
  if (const int end = TrailingSize(); end > 0) {
    x += gap;
    at.end = QRectF(x, mid - end / 2.0, end, end);
  }
  return at;
}

QSize Chip::sizeHint() const {
  const QFontMetrics metrics = fontMetrics();
  const int pad = theme::TextPx(kPaddingX), gap = theme::TextPx(kGap);
  const int icon_px = icon().isNull() ? 0 : std::min(iconSize().height(), metrics.height());
  int width = 2 * pad + metrics.horizontalAdvance(text());
  if (icon_px > 0) width += icon_px + gap;
  if (count_ >= 0) width += gap + metrics.horizontalAdvance(QString::number(count_));
  if (const int end = TrailingSize(); end > 0) width += gap + end;
  return {width, metrics.height() + 2 * theme::TextPx(kPaddingY) + 2};
}

void Chip::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  const bool on = isChecked();
  const bool enabled = isEnabled();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF box(rect());
  QColor fill = on && enabled ? tokens.accent : tokens.surface_alt;
  QColor border = on && enabled             ? tokens.accent
                  : marked_ && enabled      ? tokens.accent
                  : underMouse() && enabled ? tokens.text_muted
                                            : tokens.border;
  QColor ink = !enabled ? tokens.text_muted : on ? tokens.on_accent : tokens.text;
  QColor quiet = !enabled ? tokens.text_muted : on ? tokens.on_accent : tokens.text_muted;
  if (tint_.isValid() && enabled) {
    if (on) {
      fill = border = tint_;
      ink = InkOn(tint_);
      quiet = ink;
      quiet.setAlphaF(0.75);
    } else {
      fill = theme::Mix(tokens.surface_alt, tint_, underMouse() ? 0.22 : 0.12);
      ink = InkFor(tint_);
      border = theme::Mix(tokens.surface_alt, ink, 0.6);
      quiet = theme::Mix(ink, tokens.text_muted, 0.45);
    }
  }
  paint::Pill(painter, box, paint::PillRadius(box.height(), tokens.radius_control), fill, border);

  const Layout at = LayOut();
  if (!at.icon.isNull()) {
    icon().paint(&painter, at.icon.toRect(), Qt::AlignCenter,
                 enabled ? QIcon::Normal : QIcon::Disabled);
  }
  painter.setPen(ink);
  painter.drawText(at.text, Qt::AlignVCenter | Qt::AlignLeft, text());
  if (!at.count.isNull()) {
    painter.setPen(quiet);
    painter.drawText(at.count, Qt::AlignVCenter | Qt::AlignLeft, QString::number(count_));
  }
  if (!at.end.isNull()) {
    const icons::Glyph glyph = removable_ ? icons::Glyph::Close : icons::Glyph::ChevronDown;
    icons::For(glyph, quiet).paint(&painter, at.end.toRect());
  }
}

void Chip::changeEvent(QEvent* event) {
  QAbstractButton::changeEvent(event);
  if (event->type() == QEvent::FontChange) updateGeometry();
}

void Chip::keyPressEvent(QKeyEvent* event) {
  if (removable_ && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
      event->modifiers() == Qt::NoModifier) {
    emit RemoveClicked();
    return;
  }
  QAbstractButton::keyPressEvent(event);
}

bool Chip::event(QEvent* event) {
  // Over the ×, the tooltip says what it removes.
  if (event->type() == QEvent::ToolTip && removable_) {
    const auto* help = static_cast<QHelpEvent*>(event);
    if (LayOut().end.contains(help->pos()) && !remove_tip_.isEmpty()) {
      QToolTip::showText(help->globalPos(), remove_tip_, this);
      return true;
    }
  }
  return QAbstractButton::event(event);
}

bool Chip::hitButton(const QPoint& pos) const {
  if (removable_ && LayOut().end.adjusted(-3, -3, 3, 3).contains(pos)) return false;
  return rect().contains(pos);
}

void Chip::mousePressEvent(QMouseEvent* event) {
  if (removable_ && event->button() == Qt::LeftButton && !hitButton(event->position().toPoint())) {
    remove_pressed_ = true;
    event->accept();
    return;
  }
  QAbstractButton::mousePressEvent(event);
}

void Chip::mouseReleaseEvent(QMouseEvent* event) {
  if (remove_pressed_) {
    remove_pressed_ = false;
    if (!hitButton(event->position().toPoint()) && rect().contains(event->position().toPoint()))
      emit RemoveClicked();
    event->accept();
    return;
  }
  QAbstractButton::mouseReleaseEvent(event);
}

}  // namespace mira_gui
