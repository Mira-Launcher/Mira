#include "FocusRing.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollArea>
#include <QTimer>

#include "../theme/Theme.h"

namespace mira_gui {
namespace {

constexpr int kWidth = 2;  // the ring's line, drawn on the control's edge
constexpr char kLeftOut[] = "mira_focus_ring_left_out";

QRect InWindow(QWidget* widget, QWidget* window, const QRect& rect) {
  return {widget->mapTo(window, rect.topLeft()), rect.size()};
}

// What has focus as the eye sees it: a list's current item rather than the whole list.
QRect Target(QWidget* widget, QWidget* window) {
  if (auto* view = qobject_cast<QAbstractItemView*>(widget)) {
    const QRect item = view->visualRect(view->currentIndex());
    if (item.isValid()) return InWindow(view->viewport(), window, item);
  }
  return InWindow(widget, window, widget->rect());
}

// The part of the window `widget` shows in: inside every scroll it's in, and a list's own.
QRect Clip(QWidget* widget, QWidget* window) {
  QRect clip = window->rect();
  if (auto* view = qobject_cast<QAbstractItemView*>(widget))
    clip &= InWindow(view->viewport(), window, view->viewport()->rect());
  for (QWidget* p = widget->parentWidget(); p != nullptr && p != window; p = p->parentWidget()) {
    if (auto* area = qobject_cast<QAbstractScrollArea*>(p))
      clip &= InWindow(area->viewport(), window, area->viewport()->rect());
  }
  return clip;
}

bool NavigationKey(int key) {
  switch (key) {
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
      return true;
    default:
      return false;
  }
}

}  // namespace

// Drawn over the window, never taking a click or focus.
class FocusRing::Ring : public QWidget {
 public:
  explicit Ring(QWidget* window) : QWidget(window) {
    setObjectName("focus_ring");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
  }

  // Where there's no room around the control (a row as wide as its scroll), the ring closes inside
  // what's visible rather than losing its sides.
  void Set(const QRect& ring, const QRect& clip) {
    const QRect shown = ring & clip;
    if (shown.isEmpty()) return hide();
    // Its own repaint repaints what's under it, which asks for a place again: stop there.
    if (shown == geometry() && isVisible()) return;
    setGeometry(shown);
    update();
    raise();
    show();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const theme::Tokens& tokens = theme::Current();
    painter.setBrush(Qt::NoBrush);
    const double inset = kWidth / 2.0;
    const double radius = tokens.radius_control;
    painter.setPen(QPen(tokens.accent, kWidth));
    painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), radius, radius);
    // A backdrop-colored line inside, so the ring still shows on a control filled with the accent.
    const double inner = kWidth + 0.5;
    painter.setPen(QPen(tokens.window, 1));
    painter.drawRoundedRect(QRectF(rect()).adjusted(inner, inner, -inner, -inner), radius - kWidth,
                            radius - kWidth);
  }
};

namespace {
FocusRing* g_installed = nullptr;
}

void FocusRing::Install() {
  if (g_installed != nullptr) return;
  g_installed = new FocusRing(qApp);
}

void FocusRing::LeaveOut(QWidget* window) {
  window->setProperty(kLeftOut, true);
}

bool FocusRing::LeftOut(const QWidget* window) {
  return window->property(kLeftOut).toBool();
}

QRect FocusRing::Around(QWidget* widget) {
  return Target(widget, widget->window());
}

FocusRing::FocusRing(QObject* parent) : QObject(parent) {
  qApp->installEventFilter(this);
  connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) { Follow(now); });
}

bool FocusRing::eventFilter(QObject* watched, QEvent* event) {
  switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
      SetShown(false);
      break;
    case QEvent::KeyPress:
      if (NavigationKey(static_cast<QKeyEvent*>(event)->key())) SetShown(true);
      break;
    case QEvent::FocusIn: {
      const Qt::FocusReason reason = static_cast<QFocusEvent*>(event)->reason();
      if (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason) SetShown(true);
      break;
    }
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::Hide:
      // A scroll moves the page the control is on.
      if (shown_ && watched_.contains(watched)) PlaceSoon();
      break;
    case QEvent::Paint:
      // A list repaints as its current item moves or it scrolls.
      if (shown_ && watched == painted_) PlaceSoon();
      break;
    default:
      break;
  }
  return false;
}

void FocusRing::Follow(QWidget* focus) {
  focus_ = focus;
  watched_.clear();
  for (QWidget* w = focus; w != nullptr; w = w->parentWidget()) {
    watched_ << w;
    if (w->isWindow()) break;
  }
  auto* view = qobject_cast<QAbstractItemView*>(focus);
  painted_ = view != nullptr ? view->viewport() : nullptr;
  PlaceSoon();
}

void FocusRing::SetShown(bool shown) {
  if (shown_ == shown) return;
  shown_ = shown;
  PlaceSoon();
}

void FocusRing::PlaceSoon() {
  if (place_queued_) return;
  place_queued_ = true;
  // After the key or focus change that asked has been handled, so the ring sees where it ended up.
  QTimer::singleShot(0, this, [this] {
    place_queued_ = false;
    Place();
  });
}

void FocusRing::Place() {
  QWidget* focus = focus_;
  // A menu, a combo's list and a list whose current item is selected mark their own choice.
  // A page's own scroll has focus only to take Page Down; a ring around the whole page says
  // nothing.
  auto* view = qobject_cast<QAbstractItemView*>(focus);
  const bool marked = view != nullptr && view->selectionModel() != nullptr &&
                      view->selectionModel()->isSelected(view->currentIndex());
  const bool hidden = !shown_ || focus == nullptr || !focus->isVisible() || marked ||
                      focus->window()->inherits("QMenu") ||
                      focus->window()->inherits("QComboBoxPrivateContainer") ||
                      LeftOut(focus->window()) || qobject_cast<QScrollArea*>(focus) != nullptr;
  if (hidden) {
    if (ring_ != nullptr) ring_->hide();
    return;
  }
  QWidget* window = focus->window();
  if (ring_ == nullptr || ring_->parentWidget() != window) {
    delete ring_;
    ring_ = new Ring(window);
  }
  ring_->Set(Around(focus), Clip(focus, window));
}

}  // namespace mira_gui
