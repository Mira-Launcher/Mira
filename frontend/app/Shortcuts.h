#pragma once

#include <QList>
#include <QString>

class QAction;
class QMainWindow;
class QWidget;

namespace mira_gui::shortcuts {

// One row of the keyboard reference dialog.
struct Entry {
  QString keys;
  QString description;
};

// The actions every top-level window carries, whether or not it has a menu
// bar to put them in.
struct Common {
  QAction* quit = nullptr;
  QAction* close_window = nullptr;
  QAction* reference = nullptr;
};

// Creates the common actions and adds them to `window` itself.
//
// Adding them to the window is what makes the keys live: a QAction's
// shortcut only fires while it belongs to a widget in the focus chain. A
// window with a menu bar can still put these in a menu afterwards, and the
// same QAction in two places is the normal Qt arrangement.
//
// `window_specific` is the rest of this window's keys, listed in the
// reference dialog below the common ones.
Common Install(QMainWindow* window, const QList<Entry>& window_specific = {});

// Every key this window responds to, in one read-only list.
void ShowReference(QWidget* parent, const QList<Entry>& window_specific);

}  // namespace mira_gui::shortcuts
