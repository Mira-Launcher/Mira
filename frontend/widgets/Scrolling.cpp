#include "Scrolling.h"

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
    // The wheel scrolls the area; a dropdown, number box or slider in it only takes
    // the wheel once focused, so passing over one doesn't change it.
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
      if (control->hasFocus()) return false;
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
      case Qt::Key_PageDown: bar->triggerAction(QAbstractSlider::SliderPageStepAdd); return true;
      case Qt::Key_PageUp: bar->triggerAction(QAbstractSlider::SliderPageStepSub); return true;
      // Home and End stay a line edit's, for its cursor.
      case Qt::Key_Home:
      case Qt::Key_End:
        if (focus->inherits("QLineEdit")) return false;
        bar->triggerAction(key->key() == Qt::Key_Home ? QAbstractSlider::SliderToMinimum
                                                       : QAbstractSlider::SliderToMaximum);
        return true;
      default: return false;
    }
  }

  // Set up for a whole page, not just its own area.
  bool PageWide() const { return !scope_.isNull() && scope_ != area_; }

 private:
  QPointer<QAbstractScrollArea> area_;
  QPointer<QWidget> scope_;
};

}  // namespace

void SetUpScrolling(QAbstractScrollArea* area, QWidget* scope) {
  area->verticalScrollBar()->setSingleStep(std::max(1, kNotchPixels / std::max(1, QApplication::wheelScrollLines())));
  new PageKeys(area, scope != nullptr ? scope : area);
}

void FocusPage(QWidget* page) {
  for (QAbstractScrollArea* area : page->findChildren<QAbstractScrollArea*>()) {
    for (QObject* child : area->children()) {
      const auto* keys = dynamic_cast<const PageKeys*>(child);
      if (keys != nullptr && keys->PageWide()) {
        area->setFocus(Qt::OtherFocusReason);
        return;
      }
    }
  }
}

void SettleLayout(QWidget* page) {
  // Each layout's new height reaches the one above it as a posted request, which this runs now.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
  // A height-for-width change doesn't reach the scroll area that sizes the page by itself.
  page->updateGeometry();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
}

}  // namespace mira_gui
