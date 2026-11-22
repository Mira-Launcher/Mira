#include "SpatialNav.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QFocusEvent>
#include <QHash>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

#include "FocusRing.h"

namespace mira_gui::spatial_nav {
namespace {

bool Horizontal(int arrow) {
  return arrow == Qt::Key_Left || arrow == Qt::Key_Right;
}

QRect InWindow(QWidget* widget, QWidget* window, const QRect& rect) {
  return {widget->mapTo(window, rect.topLeft()), rect.size()};
}

// What a move starts from: a list's current item rather than the whole list. Moving off it by
// `arrow`, the item reaches to the list's edge that way, so a control beside the list never counts
// as past it however tall its items are.
QRect FromRect(QWidget* focus, QWidget* window, int arrow = 0) {
  if (auto* view = qobject_cast<QAbstractItemView*>(focus)) {
    QRect item = view->visualRect(view->currentIndex()) & view->viewport()->rect();
    if (!item.isEmpty()) {
      item = InWindow(view->viewport(), window, item);
      const QRect edge = InWindow(view, window, view->rect());
      switch (arrow) {
        case Qt::Key_Down:
          item.setBottom(edge.bottom());
          break;
        case Qt::Key_Up:
          item.setTop(edge.top());
          break;
        case Qt::Key_Right:
          item.setRight(edge.right());
          break;
        case Qt::Key_Left:
          item.setLeft(edge.left());
          break;
        default:
          break;
      }
      return item;
    }
  }
  return InWindow(focus, window, focus->rect());
}

// Whether `widget` can be seen, or scrolled into view, rather than sitting under a card or another
// window-wide overlay.
bool Reachable(QWidget* widget, QWidget* window) {
  QRect clip = window->rect();
  QWidget* scroller = nullptr;
  for (QWidget* p = widget->parentWidget(); p != nullptr && p != window; p = p->parentWidget()) {
    if (auto* area = qobject_cast<QAbstractScrollArea*>(p)) {
      clip &= InWindow(area->viewport(), window, area->viewport()->rect());
      if (scroller == nullptr) scroller = area;
    }
  }
  if (clip.isEmpty()) return false;
  const QPoint center = InWindow(widget, window, widget->rect()).center();
  const QPoint at(std::clamp(center.x(), clip.left(), clip.right()),
                  std::clamp(center.y(), clip.top(), clip.bottom()));
  QWidget* hit = window->childAt(at);
  if (hit == nullptr) return false;
  return hit == widget || widget->isAncestorOf(hit) || hit->isAncestorOf(widget) ||
         (scroller != nullptr && scroller->isAncestorOf(hit));
}

bool Focusable(QWidget* widget, QWidget* window) {
  if (!widget->isVisible() || !widget->isEnabled() || widget->window() != window) return false;
  // Qt makes only the checked button of an exclusive group a Tab stop; arrows reach them all.
  auto* button = qobject_cast<QAbstractButton*>(widget);
  const bool in_group = button != nullptr && button->isCheckable() &&
                        (button->autoExclusive() || button->group() != nullptr);
  const Qt::FocusPolicy needed = in_group ? Qt::ClickFocus : Qt::TabFocus;
  if (!(widget->focusPolicy() & needed) || widget->focusProxy() != nullptr) return false;
  // A plain scroll area holds the controls; it isn't one.
  if (qobject_cast<QScrollArea*>(widget) != nullptr) return false;
  return Reachable(widget, window);
}

// The nearest control in `within` the way `arrow` points from `f`, preferring ones in line with it.
// `from` (if any) is the control `f` is, left out with what holds it.
QWidget* Nearest(QWidget* window, QWidget* within, const QRect& f, QWidget* from, int arrow) {
  QWidget* best = nullptr;
  double best_score = std::numeric_limits<double>::max();
  for (QWidget* widget : within->findChildren<QWidget*>()) {
    if (from != nullptr &&
        (widget == from || widget->isAncestorOf(from) || from->isAncestorOf(widget))) {
      continue;
    }
    if (!Focusable(widget, window)) continue;
    const QRect c = InWindow(widget, window, widget->rect());
    int along = 0;  // how far ahead
    switch (arrow) {
      case Qt::Key_Down:
        along = c.top() - f.bottom();
        break;
      case Qt::Key_Up:
        along = f.top() - c.bottom();
        break;
      case Qt::Key_Right:
        along = c.left() - f.right();
        break;
      default:
        along = f.left() - c.right();
        break;
    }
    // Slightly overlapping still counts as ahead; a frame's border often does.
    if (along < -4) continue;
    int across = 0;  // how far out of line
    double offset = 0;
    if (Horizontal(arrow)) {
      across = std::max({0, c.top() - f.bottom(), f.top() - c.bottom()});
      offset = std::abs(c.center().y() - f.center().y());
    } else {
      across = std::max({0, c.left() - f.right(), f.left() - c.right()});
      offset = std::abs(c.center().x() - f.center().x());
    }
    const double score = std::max(along, 0) + 3.0 * across + 0.05 * offset;
    if (score < best_score) {
      best_score = score;
      best = widget;
    }
  }
  return best;
}

// The first control in `within`'s tab order: for a window with nothing focused yet, or a page whose
// scroll has focus.
QWidget* First(QWidget* window, QWidget* within) {
  for (QWidget* w = within->nextInFocusChain(); w != nullptr && w != within;
       w = w->nextInFocusChain()) {
    if (within->isAncestorOf(w) && Focusable(w, window)) return w;
  }
  return nullptr;
}

// The nearest control from `f`, first within the page `inside` is on, so a sidebar beside it is
// only reached off the page's edge.
QWidget* NearestFrom(QWidget* window, QWidget* inside, const QRect& f, QWidget* from, int arrow) {
  for (QWidget* p = inside->parentWidget(); p != nullptr && p != window; p = p->parentWidget()) {
    if (auto* area = qobject_cast<QScrollArea*>(p)) {
      if (QWidget* next = Nearest(window, area->viewport(), f, from, arrow)) return next;
      break;
    }
  }
  return Nearest(window, window, f, from, arrow);
}

// A list's current item, selected as the list's own arrows would, so its own look marks it.
void MakeCurrent(QAbstractItemView* view, const QModelIndex& index) {
  if (view->selectionModel() == nullptr || view->selectionMode() == QAbstractItemView::NoSelection)
    return view->setCurrentIndex(index);
  view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
}

void FocusOn(QWidget* widget, const QRect& from) {
  widget->setFocus(Qt::TabFocusReason);
  for (QWidget* p = widget->parentWidget(); p != nullptr; p = p->parentWidget()) {
    if (auto* area = qobject_cast<QScrollArea*>(p)) area->ensureWidgetVisible(widget, 16, 16);
  }
  // Into a list: the item beside where focus came from, so the move shows.
  auto* view = qobject_cast<QAbstractItemView*>(widget);
  if (view == nullptr || view->model() == nullptr) return;
  QWidget* window = widget->window();
  const QRect area = view->viewport()->rect();
  QPoint at = view->viewport()->mapFrom(window, from.center());
  at = {std::clamp(at.x(), area.left(), area.right()),
        std::clamp(at.y(), area.top(), area.bottom())};
  QModelIndex index = view->indexAt(at);
  if (!index.isValid() && view->currentIndex().isValid()) return;
  if (!index.isValid()) index = view->model()->index(0, 0, view->rootIndex());
  if (index.isValid()) MakeCurrent(view, index);
}

// The last mouse press, so the first arrow after it starts where the user was pointing rather than
// wherever focus was left.
struct Press {
  QPointer<QWidget> window;
  QPointer<QWidget> hit;      // what was under the pointer
  QPointer<QWidget> control;  // the control that holds it, if any
  QPoint at;                  // in the window
  bool pending = false;       // no key since
};
Press g_press;

void NotePress(QWidget* hit, const QPoint& global) {
  QWidget* window = hit->window();
  if (window->windowType() == Qt::Popup || FocusRing::LeftOut(window)) {
    g_press.pending = false;
    return;
  }
  QWidget* control = hit;
  while (control != nullptr && control != window &&
         (!(control->focusPolicy() & Qt::ClickFocus) || qobject_cast<QScrollArea*>(control)))
    control = control->parentWidget();
  if (control == window) control = nullptr;
  g_press = {window, hit, control, window->mapFromGlobal(global), true};
}

// After a click, the first arrow lands on the control clicked (or the one now at that spot) when
// focus went elsewhere, and from a click on nothing moves from that spot. False when focus already
// is what was clicked, so the arrow just moves on.
bool MoveFromPress(QWidget* window, QWidget* focus, int arrow) {
  if (!g_press.pending) return false;
  g_press.pending = false;
  if (g_press.window != window || g_press.hit == nullptr) return false;
  QWidget* control = g_press.control;
  if (control != nullptr && (control == focus || control->isAncestorOf(focus))) return false;
  if (control == nullptr || !Focusable(control, window)) {
    control = nullptr;
    for (QWidget* w = window->childAt(g_press.at); w != nullptr && w != window;
         w = w->parentWidget()) {
      if (Focusable(w, window)) {
        control = w;
        break;
      }
    }
  }
  if (control != nullptr && control == focus) return false;
  const QRect spot(g_press.at, QSize(1, 1));
  if (control == nullptr) control = NearestFrom(window, g_press.hit, spot, nullptr, arrow);
  if (control == nullptr) return false;
  FocusOn(control, spot);
  return true;
}

constexpr char kTabLast[] = "mira_tab_last";

// `parent`'s children in reading order: rows top to bottom (a child starting above the middle of a
// row's first one joins that row), left to right within a row, marked ones last.
QHash<QWidget*, int> ReadingRanks(QWidget* parent) {
  QList<QWidget*> children = parent->findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
  std::stable_sort(children.begin(), children.end(),
                   [](QWidget* a, QWidget* b) { return a->y() < b->y(); });
  std::vector<std::pair<std::tuple<bool, int, int>, QWidget*>> keyed;
  int row = 0;
  int row_middle = std::numeric_limits<int>::min();
  for (QWidget* child : children) {
    if (child->y() >= row_middle) {
      ++row;
      row_middle = child->y() + child->height() / 2;
    }
    keyed.push_back({{child->property(kTabLast).toBool(), row, child->x()}, child});
  }
  std::stable_sort(keyed.begin(), keyed.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  QHash<QWidget*, int> ranks;
  for (size_t i = 0; i < keyed.size(); ++i) ranks.insert(keyed[i].second, static_cast<int>(i));
  return ranks;
}

// Relinks `window`'s Tab chain into reading order, just before Tab uses it, so rows built after
// the rest (a sidebar's sources, a page's cards) still fall in place.
void OrderTabs(QWidget* window) {
  QHash<QWidget*, QHash<QWidget*, int>> ranks;  // per container
  std::vector<std::pair<std::vector<int>, QWidget*>> stops;
  for (QWidget* widget : window->findChildren<QWidget*>()) {
    if (widget->window() != window || !(widget->focusPolicy() & Qt::TabFocus)) continue;
    std::vector<int> path;
    for (QWidget* w = widget; w != window; w = w->parentWidget()) {
      QWidget* parent = w->parentWidget();
      auto found = ranks.find(parent);
      if (found == ranks.end()) found = ranks.insert(parent, ReadingRanks(parent));
      path.push_back(found->value(w));
    }
    std::reverse(path.begin(), path.end());
    stops.push_back({std::move(path), widget});
  }
  std::stable_sort(stops.begin(), stops.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t i = 1; i < stops.size(); ++i)
    QWidget::setTabOrder(stops[i - 1].second, stops[i].second);
}

// After Tab moved focus: on past controls under a card, as the arrows never land there.
void SkipCoveredSoon(QWidget* window, int key) {
  QTimer::singleShot(0, window, [window, key] {
    QWidget* focus = QApplication::focusWidget();
    if (focus == nullptr || focus->window() != window || Reachable(focus, window)) return;
    const bool forward = key == Qt::Key_Tab;
    for (QWidget* w = forward ? focus->nextInFocusChain() : focus->previousInFocusChain();
         w != nullptr && w != focus;
         w = forward ? w->nextInFocusChain() : w->previousInFocusChain()) {
      if (w->window() == window && w->isVisible() && w->isEnabled() &&
          (w->focusPolicy() & Qt::TabFocus) && w->focusProxy() == nullptr && Reachable(w, window))
        return w->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    }
  });
}

// Whether `arrow` moves focus off `widget` rather than going to it. A slider keeps the arrows
// along it, a text field keeps Left and Right until its cursor can't go further.
bool MovesOff(QWidget* widget, int arrow) {
  if (qobject_cast<QAbstractButton*>(widget) != nullptr) return true;
  if (auto* combo = qobject_cast<QComboBox*>(widget))
    return !combo->isEditable() || !Horizontal(arrow);
  if (qobject_cast<QAbstractSpinBox*>(widget) != nullptr) return !Horizontal(arrow);
  if (auto* slider = qobject_cast<QAbstractSlider*>(widget))
    return (slider->orientation() == Qt::Horizontal) != Horizontal(arrow);
  auto* edit = qobject_cast<QLineEdit*>(widget);
  if (edit == nullptr || edit->hasSelectedText()) return false;
  return (arrow == Qt::Key_Left && edit->cursorPosition() == 0) ||
         (arrow == Qt::Key_Right && edit->cursorPosition() == edit->text().size());
}

// Takes the arrows nothing on the way to the window used, and those a page would only scroll by.
class Keys : public QObject {
 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    // A page's scroll takes focus on a click, but isn't a Tab stop.
    if (event->type() == QEvent::Polish) {
      auto* area = qobject_cast<QScrollArea*>(watched);
      if (area != nullptr && (area->focusPolicy() & Qt::TabFocus))
        area->setFocusPolicy(Qt::ClickFocus);
      return false;
    }
    // Tabbing into a list: its current item (else its first) selected, so focus shows on a game
    // and Enter has one to act on.
    if (event->type() == QEvent::FocusIn) {
      const Qt::FocusReason reason = static_cast<QFocusEvent*>(event)->reason();
      auto* view = qobject_cast<QAbstractItemView*>(watched);
      if (view == nullptr || (reason != Qt::TabFocusReason && reason != Qt::BacktabFocusReason))
        return false;
      QTimer::singleShot(0, view, [view] {
        if (view->model() == nullptr || !view->hasFocus()) return;
        QModelIndex index = view->currentIndex();
        if (!index.isValid()) index = view->model()->index(0, 0, view->rootIndex());
        if (!index.isValid() || view->selectionModel() == nullptr ||
            view->selectionModel()->isSelected(index))
          return;
        MakeCurrent(view, index);
      });
      return false;
    }
    if (event->type() == QEvent::MouseButtonPress && watched->isWidgetType()) {
      const QPoint global = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
      if (QWidget* hit = QApplication::widgetAt(global)) NotePress(hit, global);
      return false;
    }
    if (event->type() != QEvent::KeyPress) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    const int arrow = key->key();
    // Enter presses the focused button, as Space does; Qt leaves it to a dialog's default button.
    if ((arrow == Qt::Key_Return || arrow == Qt::Key_Enter) &&
        (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
      g_press.pending = false;
      auto* button = qobject_cast<QAbstractButton*>(watched);
      auto* push = qobject_cast<QPushButton*>(watched);
      if (button == nullptr || !button->hasFocus() || !button->isEnabled() ||
          (push != nullptr && (push->autoDefault() || push->isDefault())) ||
          FocusRing::LeftOut(button->window()))
        return false;
      button->animateClick();
      key->accept();
      return true;
    }
    if (arrow == Qt::Key_Tab || arrow == Qt::Key_Backtab) {
      g_press.pending = false;
      auto* widget = qobject_cast<QWidget*>(watched);
      if (widget != nullptr && widget->hasFocus() && !FocusRing::LeftOut(widget->window())) {
        OrderTabs(widget->window());
        SkipCoveredSoon(widget->window(), arrow);
      }
      return false;
    }
    if (arrow != Qt::Key_Up && arrow != Qt::Key_Down && arrow != Qt::Key_Left &&
        arrow != Qt::Key_Right) {
      g_press.pending = false;
      return false;
    }
    if ((key->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier) return false;
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) return false;
    QWidget* window = widget->window();
    if (FocusRing::LeftOut(window)) return false;
    // At the focused control, before it can use the arrow itself.
    if (widget->hasFocus() && MoveFromPress(window, widget, arrow)) {
      key->accept();
      return true;
    }
    // Arrows move focus off a control rather than change it, so passing over a page changes
    // nothing: a button would step through the tab order or pick within its group, and a dropdown
    // or number box would change its value. Space and Enter still press or open them.
    if (widget->hasFocus() && MovesOff(widget, arrow)) {
      Move(window, widget, arrow);
      key->accept();
      return true;
    }
    if (!widget->isWindow() && qobject_cast<QScrollArea*>(widget) == nullptr) return false;
    QWidget* focus = QApplication::focusWidget();
    if (!Move(window, focus != nullptr && focus->window() == window ? focus : nullptr, arrow))
      return false;
    key->accept();
    return true;
  }
};

}  // namespace

void Install() {
  static Keys* keys = nullptr;
  if (keys != nullptr) return;
  keys = new Keys();
  keys->setParent(qApp);
  qApp->installEventFilter(keys);
}

void TabLast(QWidget* widget) {
  widget->setProperty(kTabLast, true);
}

bool Move(QWidget* window, QWidget* focus, int arrow) {
  if (MoveFromPress(window, focus, arrow)) return true;
  if (focus == nullptr || qobject_cast<QScrollArea*>(focus) != nullptr) {
    QWidget* first = First(window, focus != nullptr ? focus : window);
    if (first != nullptr) FocusOn(first, FromRect(first, window));
    return first != nullptr;
  }
  QWidget* next = NearestFrom(window, focus, FromRect(focus, window, arrow), focus, arrow);
  if (next != nullptr) FocusOn(next, FromRect(focus, window));
  return next != nullptr;
}

}  // namespace mira_gui::spatial_nav
