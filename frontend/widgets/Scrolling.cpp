#include "Scrolling.h"

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTextEdit>
#include <algorithm>

namespace mira_gui {
namespace {

constexpr int kNotchPixels = 120;

// One per area, owned by it, watching the whole app: a key goes to the focused
// widget first, so that's the only point that sees it whatever has focus.
class PageKeys : public QObject {
 public:
  PageKeys(QAbstractScrollArea* area, QWidget* scope) : QObject(area), area_(area), scope_(scope) {
    qApp->installEventFilter(this);
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() != QEvent::KeyPress || area_.isNull() || scope_.isNull()) return false;
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

 private:
  QPointer<QAbstractScrollArea> area_;
  QPointer<QWidget> scope_;
};

}  // namespace

void SetUpScrolling(QAbstractScrollArea* area, QWidget* scope) {
  area->verticalScrollBar()->setSingleStep(std::max(1, kNotchPixels / std::max(1, QApplication::wheelScrollLines())));
  new PageKeys(area, scope != nullptr ? scope : area);
}

}  // namespace mira_gui
