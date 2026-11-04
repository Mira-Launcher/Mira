#include "DesktopController.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QTabBar>

#include "../bigscreen/GamepadInput.h"
#include "../widgets/SpatialNav.h"

namespace mira_gui {
namespace {

using bigscreen::Nav;

// Longer than this, a Guide press is a hold, not a tap. A controller that turns off with Guide
// still down lets go only once it's back, long after.
constexpr qint64 kGuideTapMs = 1500;

bool Horizontal(Nav nav) {
  return nav == Nav::Left || nav == Nav::Right;
}

int ArrowKey(Nav nav) {
  switch (nav) {
    case Nav::Up:
      return Qt::Key_Up;
    case Nav::Down:
      return Qt::Key_Down;
    case Nav::Left:
      return Qt::Key_Left;
    default:
      return Qt::Key_Right;
  }
}

// Where a key goes: an open menu or combo list, else the focused widget, else the window.
QWidget* KeyTarget() {
  if (QWidget* popup = QApplication::activePopupWidget())
    return popup->focusWidget() != nullptr ? popup->focusWidget() : popup;
  if (QWidget* focus = QApplication::focusWidget()) return focus;
  return QApplication::activeWindow();
}

// A key sent from here still reaches the window's shortcuts first, as a typed one would: Qt runs
// the shortcut map for every key event that isn't spontaneous.
void SendKey(QWidget* target, int key, Qt::KeyboardModifiers modifiers = {}) {
  QPointer<QWidget> guard(target);
  QKeyEvent press(QEvent::KeyPress, key, modifiers);
  QApplication::sendEvent(target, &press);
  if (guard == nullptr) return;
  QKeyEvent release(QEvent::KeyRelease, key, modifiers);
  QApplication::sendEvent(target, &release);
}

void PressKey(int key, Qt::KeyboardModifiers modifiers = {}) {
  if (QWidget* target = KeyTarget()) SendKey(target, key, modifiers);
}

// The arrow key, as the keyboard sends it, where the control moves through what it holds: a list's
// items, a tab bar's tabs, a slider's value. At a list's edge the key moves focus on. Anywhere
// else focus moves straight away, so a text box's cursor or a drop-down's value never holds the
// stick.
void Move(Nav nav) {
  const int arrow = ArrowKey(nav);
  if (QApplication::activePopupWidget() != nullptr) return PressKey(arrow);
  QWidget* window = QApplication::activeWindow();
  if (window == nullptr) return;
  QWidget* focus = QApplication::focusWidget();
  if (focus != nullptr && focus->window() != window) focus = nullptr;
  const bool steers = qobject_cast<QAbstractItemView*>(focus) != nullptr ||
                      (Horizontal(nav) && (qobject_cast<QTabBar*>(focus) != nullptr ||
                                           qobject_cast<QAbstractSlider*>(focus) != nullptr));
  if (steers) return SendKey(focus, arrow);
  if (auto* spin = qobject_cast<QAbstractSpinBox*>(focus); spin != nullptr && Horizontal(nav))
    return spin->stepBy(nav == Nav::Right ? 1 : -1);
  spatial_nav::Move(window, focus, arrow);
}

void Accept() {
  if (QApplication::activePopupWidget() != nullptr) return PressKey(Qt::Key_Return);
  QWidget* focus = QApplication::focusWidget();
  if (auto* combo = qobject_cast<QComboBox*>(focus)) return combo->showPopup();
  if (auto* button = qobject_cast<QAbstractButton*>(focus)) return button->animateClick();
  PressKey(Qt::Key_Return);
}

// The focused control's right-click menu, at its current item for a list.
void ContextMenu() {
  QWidget* focus = QApplication::focusWidget();
  if (focus == nullptr || QApplication::activePopupWidget() != nullptr) return;
  QWidget* receiver = focus;
  QPoint at = focus->rect().center();
  if (auto* view = qobject_cast<QAbstractItemView*>(focus)) {
    receiver = view->viewport();
    const QRect item = view->visualRect(view->currentIndex());
    at = item.isValid() ? item.center() : receiver->rect().center();
  }
  QContextMenuEvent event(QContextMenuEvent::Keyboard, at, receiver->mapToGlobal(at));
  QApplication::sendEvent(receiver, &event);
}

// Big screen's settings, plus Start (Menu) as a right click as well as X.
bigscreen::GamepadInput::Options DesktopOptions(bigscreen::GamepadInput::Options options) {
  options.start_as_action = true;
  return options;
}

}  // namespace

DesktopController::DesktopController(QObject* parent, bool in_background)
    : QObject(parent), options_(DesktopOptions({})) {
  if (in_background) {
    bigscreen::GamepadInput::CreateInBackground(
        this, [this](bigscreen::GamepadInput* input) { Start(input); });
  } else {
    Start(new bigscreen::GamepadInput(this));
  }
}

void DesktopController::Start(bigscreen::GamepadInput* input) {
  input_ = input;
  // Queued: a press can open a menu or dialog with exec(), whose loop must not run inside the poll,
  // since Qt never fires a timer again while its timeout is still running.
  connect(input_, &bigscreen::GamepadInput::Pressed, this, &DesktopController::Handle,
          Qt::QueuedConnection);
  // On release, so the Guide press isn't still down as big screen opens, and only after a tap:
  // holding Guide is how many controllers turn off.
  connect(
      input_, &bigscreen::GamepadInput::Released, this,
      [this](Nav nav) {
        const bool tapped = guide_down_.isValid() && guide_down_.elapsed() < kGuideTapMs;
        if (nav == Nav::Guide) guide_down_.invalidate();
        if (nav == Nav::Guide && tapped && !big_screen_open_ &&
            QApplication::activeWindow() != nullptr)
          emit OpenBigScreen();
      },
      Qt::QueuedConnection);
  connect(qApp, &QGuiApplication::applicationStateChanged, this, &DesktopController::UpdatePaused);
  input_->SetOptions(options_);
  UpdatePaused();
}

void DesktopController::ApplyPrefs(const FrontendPrefs& prefs) {
  options_ = DesktopOptions(bigscreen::GamepadInput::OptionsFrom(prefs));
  if (input_ != nullptr) input_->SetOptions(options_);
}

void DesktopController::SetBigScreenOpen(bool open) {
  big_screen_open_ = open;
  UpdatePaused();
}

void DesktopController::UpdatePaused() {
  if (input_ == nullptr) return;
  input_->SetPaused(big_screen_open_ ||
                    QGuiApplication::applicationState() != Qt::ApplicationActive);
}

void DesktopController::Handle(Nav nav) {
  if (nav == Nav::Guide) guide_down_.start();
  if (big_screen_open_ || QApplication::activeWindow() == nullptr) return;
  switch (nav) {
    case Nav::Up:
    case Nav::Down:
    case Nav::Left:
    case Nav::Right:
      return Move(nav);
    case Nav::Accept:
      return Accept();
    case Nav::Back:
      return PressKey(Qt::Key_Escape);
    case Nav::Action:
      return ContextMenu();
    case Nav::Search:
      return PressKey(Qt::Key_F, Qt::ControlModifier);
    case Nav::PrevTab:
      return PressKey(Qt::Key_Backtab, Qt::ShiftModifier);
    case Nav::NextTab:
      return PressKey(Qt::Key_Tab);
    case Nav::PrevLetter:
      return PressKey(Qt::Key_PageUp);
    case Nav::NextLetter:
      return PressKey(Qt::Key_PageDown);
    default:
      return;
  }
}

}  // namespace mira_gui
