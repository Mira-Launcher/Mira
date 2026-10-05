#include "Tray.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMainWindow>
#include <QMenu>
#include <QSystemTrayIcon>

namespace mira_gui::tray {
namespace {

QSystemTrayIcon* g_icon = nullptr;
QMainWindow* g_window = nullptr;
bool g_quitting = false;

void Restore() {
  if (g_window == nullptr) return;
  g_window->show();
  g_window->raise();
  g_window->activateWindow();
}

}  // namespace

bool Available() { return g_icon != nullptr; }

bool IsManaged(QMainWindow* window) { return window != nullptr && window == g_window; }

bool Quitting() { return g_quitting; }

void RequestQuit() {
  g_quitting = true;
  QApplication::closeAllWindows();
  // A window that refused to close (cancelled, or saving first) keeps the app alive.
  const QWidgetList windows = QApplication::topLevelWidgets();
  const bool refused = std::ranges::any_of(windows, [](QWidget* widget) {
    return qobject_cast<QMainWindow*>(widget) != nullptr && widget->isVisible();
  });
  if (refused) {
    g_quitting = false;
    return;
  }
  QApplication::quit();  // needed since Attach() disables quitOnLastWindowClosed
}

void Attach(QMainWindow* window) {
  if (g_icon != nullptr || window == nullptr) return;  // one per process
  if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

  g_window = window;
  // Otherwise Qt quits when hide-to-tray drops visible windows to zero.
  QApplication::setQuitOnLastWindowClosed(false);

  g_icon = new QSystemTrayIcon(QApplication::windowIcon(), window);
  g_icon->setToolTip("Mira");

  auto* menu = new QMenu(window);
  QAction* toggle = menu->addAction("Show/Hide Mira");
  QObject::connect(toggle, &QAction::triggered, toggle, [] {
    if (g_window == nullptr) return;
    if (g_window->isVisible()) {
      g_window->close();  // not hide(): closeEvent hides it and saves its layout
    } else {
      Restore();
    }
  });
  menu->addSeparator();
  menu->addAction("&Quit", &RequestQuit);
  g_icon->setContextMenu(menu);

  // Relabelled on each open: "Show/Hide" alone reads as unfinished.
  QObject::connect(menu, &QMenu::aboutToShow, toggle, [toggle] {
    toggle->setText(g_window != nullptr && g_window->isVisible() ? "Hide Mira" : "Show Mira");
  });

  // Trigger (left click) or DoubleClick, since platforms differ on which
  // one fires for a tray icon.
  QObject::connect(g_icon, &QSystemTrayIcon::activated, window,
                   [](QSystemTrayIcon::ActivationReason reason) {
                     if (reason == QSystemTrayIcon::Trigger ||
                         reason == QSystemTrayIcon::DoubleClick) {
                       Restore();
                     }
                   });

  g_icon->show();
}

}  // namespace mira_gui::tray
