#pragma once

#include <doctest.h>

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTest>
#include <memory>

#include "widgets/FocusRing.h"
#include "widgets/SpatialNav.h"

namespace nav_test {

// One app for every test that opens a window (offscreen).
inline QApplication& App() {
  static int argc = 1;
  static char name[] = "mira_gui_tests";
  static char* argv[] = {name, nullptr};
  static std::unique_ptr<QApplication> app = [] {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    return std::make_unique<QApplication>(argc, argv);
  }();
  return *app;
}

// A window laid out like the library: a search box over a grid of games, a button beside the
// grid and two under it, and Esc and Ctrl+F as window actions.
struct Library {
  QApplication& app = App();  // before any widget
  QWidget window;
  QLineEdit* search = new QLineEdit(&window);
  QListWidget* grid = new QListWidget(&window);
  QPushButton* settings = new QPushButton("Settings", &window);
  QPushButton* play = new QPushButton("Play", &window);
  QPushButton* remove = new QPushButton("Remove", &window);
  int escapes = 0;
  int removes = 0;

  Library() {
    window.setGeometry(100, 100, 600, 400);
    search->setGeometry(10, 10, 300, 30);
    grid->setGeometry(10, 50, 330, 220);
    grid->setViewMode(QListView::IconMode);
    grid->setGridSize({100, 100});
    grid->setMovement(QListView::Static);
    grid->setResizeMode(QListView::Adjust);
    // One row of three, so Right from the last one can only go on to Settings.
    for (const char* game : {"Celeste", "Hollow Knight", "Balatro"}) grid->addItem(game);
    settings->setGeometry(360, 60, 100, 30);
    play->setGeometry(10, 290, 100, 30);
    remove->setGeometry(120, 290, 100, 30);
    QObject::connect(remove, &QPushButton::clicked, &window, [this] { ++removes; });
    auto* escape = new QAction(&window);
    escape->setShortcut(Qt::Key_Escape);
    QObject::connect(escape, &QAction::triggered, &window, [this] { ++escapes; });
    window.addAction(escape);
    auto* find = new QAction(&window);
    find->setShortcut(QKeySequence::Find);
    QObject::connect(find, &QAction::triggered, search, [this] { search->setFocus(); });
    window.addAction(find);
    // As main() sets them up.
    mira_gui::FocusRing::Install();
    mira_gui::spatial_nav::Install();
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
  }
};

}  // namespace nav_test
