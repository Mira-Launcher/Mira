#include "ShortcutEdit.h"

#include <QKeyEvent>
#include <QPainter>

#include "../theme/Theme.h"

namespace mira_gui {
namespace {

constexpr int kCapHeight = 22;
constexpr int kCapPadding = 7;
constexpr int kCapGap = 4;

}  // namespace

ShortcutEdit::ShortcutEdit(const QKeySequence& keys, QWidget* parent) : QWidget(parent), keys_(keys) {
  setFocusPolicy(Qt::StrongFocus);
  setCursor(Qt::PointingHandCursor);
  setToolTip("Click, then press the new shortcut");
  setAttribute(Qt::WA_Hover);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setMinimumWidth(150);  // so recording doesn't resize the row
}

void ShortcutEdit::SetKeys(const QKeySequence& keys) {
  keys_ = keys;
  updateGeometry();
  update();
}

QStringList ShortcutEdit::Caps() const {
  if (keys_.isEmpty()) return {};
  // Built from the combination, not by splitting the text, which "Ctrl++" would break.
  const QKeyCombination combination = keys_[0];
  const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
  QStringList caps;
  if (modifiers & Qt::ControlModifier) caps << "Ctrl";
  if (modifiers & Qt::AltModifier) caps << "Alt";
  if (modifiers & Qt::ShiftModifier) caps << "Shift";
  if (modifiers & Qt::MetaModifier) caps << "Meta";
  caps << QKeySequence(combination.key()).toString(QKeySequence::NativeText);
  return caps;
}

QSize ShortcutEdit::sizeHint() const {
  const QFontMetrics metrics(font());
  if (recording_) return {metrics.horizontalAdvance("Press a shortcut") + 20, kCapHeight + 8};
  const QStringList caps = Caps();
  if (caps.isEmpty()) return {metrics.horizontalAdvance("None") + 20, kCapHeight + 8};
  int width = 8;
  for (const QString& cap : caps) width += metrics.horizontalAdvance(cap) + 2 * kCapPadding + kCapGap;
  return {width + 4, kCapHeight + 8};
}

void ShortcutEdit::paintEvent(QPaintEvent*) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  if (recording_ || hasFocus() || underMouse()) {
    painter.setPen(QPen(recording_ || hasFocus() ? tokens.accent : tokens.border, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame, tokens.radius_control, tokens.radius_control);
  }
  if (recording_) {
    painter.setPen(tokens.accent);
    painter.drawText(rect(), Qt::AlignCenter, "Press a shortcut");
    return;
  }
  const QStringList caps = Caps();
  if (caps.isEmpty()) {
    painter.setPen(tokens.text_muted);
    painter.drawText(rect(), Qt::AlignCenter, "None");
    return;
  }
  const QFontMetrics metrics(font());
  // Right-aligned, like every other control in a row.
  int width = -kCapGap;
  for (const QString& cap : caps) width += metrics.horizontalAdvance(cap) + 2 * kCapPadding + kCapGap;
  qreal x = this->width() - 4 - width;
  const qreal top = (height() - kCapHeight) / 2.0;
  for (const QString& cap : caps) {
    const qreal cap_width = metrics.horizontalAdvance(cap) + 2 * kCapPadding;
    const QRectF key(x, top, cap_width, kCapHeight);
    painter.setPen(QPen(tokens.border, 1));
    painter.setBrush(tokens.surface_alt);
    painter.drawRoundedRect(key.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    // The thicker lower edge that makes it read as a key.
    painter.drawLine(QPointF(key.left() + 3, key.bottom() - 0.5), QPointF(key.right() - 3, key.bottom() - 0.5));
    painter.setPen(tokens.text);
    painter.drawText(key, Qt::AlignCenter, cap);
    x += cap_width + kCapGap;
  }
}

void ShortcutEdit::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) SetRecording(!recording_);
}

bool ShortcutEdit::event(QEvent* event) {
  // While recording, a key that is someone's shortcut (Ctrl+F, Tab) comes here instead.
  if (recording_ && event->type() == QEvent::ShortcutOverride) {
    event->accept();
    return true;
  }
  if (recording_ && event->type() == QEvent::KeyPress) {
    keyPressEvent(static_cast<QKeyEvent*>(event));
    return true;
  }
  return QWidget::event(event);
}

void ShortcutEdit::keyPressEvent(QKeyEvent* event) {
  if (!recording_) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
      SetRecording(true);
      return;
    }
    QWidget::keyPressEvent(event);
    return;
  }
  const int key = event->key();
  if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta ||
      key == Qt::Key_unknown) {
    return;  // still waiting for the key itself
  }
  if (key == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
    SetRecording(false);
    return;
  }
  const QKeySequence keys = key == Qt::Key_Backspace && event->modifiers() == Qt::NoModifier
                                ? QKeySequence()
                                : QKeySequence(QKeyCombination(event->modifiers(), Qt::Key(key)));
  SetRecording(false);
  if (keys != keys_) {
    SetKeys(keys);
    emit Changed(keys_);
  }
}

void ShortcutEdit::focusOutEvent(QFocusEvent* event) {
  SetRecording(false);
  QWidget::focusOutEvent(event);
}

void ShortcutEdit::SetRecording(bool recording) {
  recording_ = recording;
  if (recording) setFocus(Qt::MouseFocusReason);
  updateGeometry();
  update();
}

}  // namespace mira_gui
