#include "Scrolling.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QSlider>
#include <QTextEdit>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <algorithm>

namespace mira_gui {
namespace {

constexpr int kNotchPixels = 120;

// The scroll area nearest `widget`, so a control in a nested area is that area's alone.
QAbstractScrollArea* InnerArea(QWidget* widget) {
  for (QWidget* at = widget->parentWidget(); at != nullptr; at = at->parentWidget()) {
    if (auto* area = qobject_cast<QAbstractScrollArea*>(at)) return area;
  }
  return nullptr;
}

// One per area, owned by it, watching the whole app: a key goes to the focused
// widget first, so that's the only point that sees it whatever has focus.
class PageKeys : public QObject {
 public:
  PageKeys(QAbstractScrollArea* area, QWidget* scope) : QObject(area), area_(area), scope_(scope) {
    qApp->installEventFilter(this);
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (area_.isNull() || scope_.isNull()) return false;
    // The wheel always scrolls the area: a dropdown, number box or slider in it never takes it,
    // even focused, so scrolling past one after an edit doesn't change it. Keys and clicks still
    // do.
    if (event->type() == QEvent::Wheel || event->type() == QEvent::Polish) {
      auto* control = qobject_cast<QWidget*>(watched);
      if (control == nullptr || InnerArea(control) != area_ ||
          !(qobject_cast<QComboBox*>(control) || qobject_cast<QAbstractSpinBox*>(control) ||
            qobject_cast<QSlider*>(control))) {
        return false;
      }
      if (event->type() == QEvent::Polish) {
        control->setFocusPolicy(Qt::StrongFocus);  // the wheel must not focus it on the way past
        return false;
      }
      QCoreApplication::sendEvent(area_->verticalScrollBar(), event);
      return true;
    }
    if (event->type() != QEvent::KeyPress) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->modifiers() & ~Qt::KeypadModifier) return false;
    QWidget* focus = QApplication::focusWidget();
    if (watched != focus || focus == nullptr || !scope_->isVisible()) return false;
    if (focus != scope_ && !scope_->isAncestorOf(focus)) return false;
    // These use the keys themselves.
    if (qobject_cast<QAbstractSpinBox*>(focus) || qobject_cast<QPlainTextEdit*>(focus) ||
        qobject_cast<QTextEdit*>(focus)) {
      return false;
    }
    QScrollBar* bar = area_->verticalScrollBar();
    switch (key->key()) {
      case Qt::Key_PageDown:
        bar->triggerAction(QAbstractSlider::SliderPageStepAdd);
        return true;
      case Qt::Key_PageUp:
        bar->triggerAction(QAbstractSlider::SliderPageStepSub);
        return true;
      // Home and End stay a line edit's, for its cursor.
      case Qt::Key_Home:
      case Qt::Key_End:
        if (focus->inherits("QLineEdit")) return false;
        bar->triggerAction(key->key() == Qt::Key_Home ? QAbstractSlider::SliderToMinimum
                                                      : QAbstractSlider::SliderToMaximum);
        return true;
      default:
        return false;
    }
  }

 private:
  QPointer<QAbstractScrollArea> area_;
  QPointer<QWidget> scope_;
};

// One scroll bar's glide toward where its wheel notches so far would have put it.
class Glide : public QVariantAnimation {
 public:
  static constexpr const char* kName = "wheel_glide";

  explicit Glide(QScrollBar* bar) : QVariantAnimation(bar), bar_(bar) {
    setObjectName(kName);
    setDuration(200);
    setEasingCurve(QEasingCurve::OutCubic);
    connect(this, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { bar_->setValue(value.toInt()); });
    // Dragging the bar or a key takes over.
    connect(bar, &QScrollBar::sliderPressed, this, &QVariantAnimation::stop);
    connect(bar, &QScrollBar::actionTriggered, this, &QVariantAnimation::stop);
  }

  // False when the bar is at that end already, so the wheel goes on to whatever holds it.
  bool By(int pixels) {
    const bool running = state() == QAbstractAnimation::Running;
    const int to =
        std::clamp((running ? target_ : bar_->value()) + pixels, bar_->minimum(), bar_->maximum());
    if (!running && to == bar_->value()) return false;
    target_ = to;
    stop();
    setStartValue(bar_->value());
    setEndValue(to);
    start();
    return true;
  }

 private:
  QScrollBar* bar_;
  int target_ = 0;
};

// A touchpad's own pixel steps are smooth already. On Wayland only a touchpad's scrolls come with
// phases and as synthesized by the system: a mouse wheel sends pixels too, and Qt gives every
// pointer event the seat's one device, which is "touchpad" once the compositor offers gestures.
bool Touchpad(const QWheelEvent& wheel) {
  if (wheel.phase() != Qt::NoScrollPhase || wheel.source() == Qt::MouseEventSynthesizedBySystem)
    return true;
  static const bool wayland = QGuiApplication::platformName().startsWith("wayland");
  if (wayland) return false;
  const QPointingDevice* device = wheel.pointingDevice();
  return (device != nullptr && device->type() == QInputDevice::DeviceType::TouchPad) ||
         !wheel.pixelDelta().isNull();
}

// Every scroll area hands its wheel to its vertical bar, so the bar is where a notch's jump
// becomes a glide, after the view itself has seen the wheel (Ctrl zooms, hovers end).
class SmoothWheel : public QObject {
 public:
  using QObject::QObject;

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() != QEvent::Wheel) return false;
    auto* bar = qobject_cast<QScrollBar*>(watched);
    if (bar == nullptr || bar->orientation() != Qt::Vertical) return false;
    auto* wheel = static_cast<QWheelEvent*>(event);
    const QPoint angle = wheel->angleDelta();
    if (angle.y() == 0 || angle.x() != 0 || wheel->modifiers() != Qt::NoModifier ||
        Touchpad(*wheel))
      return false;
    QAbstractScrollArea* area = InnerArea(bar);
    if (area == nullptr || area->verticalScrollBar() != bar) return false;
    // Their bars count items or lines, not pixels.
    if (auto* view = qobject_cast<QAbstractItemView*>(area);
        (view != nullptr && view->verticalScrollMode() == QAbstractItemView::ScrollPerItem) ||
        qobject_cast<QPlainTextEdit*>(area) != nullptr) {
      return false;
    }
    auto* glide = static_cast<Glide*>(
        bar->findChild<QVariantAnimation*>(Glide::kName, Qt::FindDirectChildrenOnly));
    if (glide == nullptr) glide = new Glide(bar);
    // As far as Qt's own step would have gone.
    const int pixels = qRound(angle.y() / double(kNotchPixels) * QApplication::wheelScrollLines() *
                              bar->singleStep() * (bar->invertedControls() ? -1 : 1));
    if (!glide->By(pixels)) return false;
    wheel->accept();
    return true;
  }
};

}  // namespace

void SetUpSmoothWheel() {
  qApp->installEventFilter(new SmoothWheel(qApp));
}

void SetUpScrolling(QAbstractScrollArea* area, QWidget* scope) {
  area->verticalScrollBar()->setSingleStep(
      std::max(1, kNotchPixels / std::max(1, QApplication::wheelScrollLines())));
  new PageKeys(area, scope != nullptr ? scope : area);
}

void SettleLayout(QWidget* page) {
  // Each layout's new height reaches the one above it as a posted request, which this runs now.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
  // A height-for-width change doesn't reach the scroll area that sizes the page by itself.
  page->updateGeometry();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
}

}  // namespace mira_gui
