#pragma once

#include <QList>
#include <QString>
#include <functional>

class QAction;
class QMainWindow;
class QWidget;

namespace mira_gui::shortcuts {

// One row of the keyboard reference.
struct Entry {
  QString keys;
  QString description;
  QString id = {};     // the keybindings id, so a rebound key shows as rebound
  QString group = {};  // the heading it's listed under ("Library", "View")
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
// reference ahead of the common ones. `open_settings` opens Settings on its Shortcuts page,
// for the reference's Change… button (none without it).
Common Install(QMainWindow* window, const QList<Entry>& window_specific = {},
               std::function<void()> open_settings = {});

// Every key this window responds to, in one read-only card over it.
void ShowReference(QWidget* parent, const QList<Entry>& window_specific,
                   const std::function<void()>& open_settings);

// "(Ctrl+H shows hidden games)", with whatever key that is bound to now.
QString HiddenGamesHint();

}  // namespace mira_gui::shortcuts
