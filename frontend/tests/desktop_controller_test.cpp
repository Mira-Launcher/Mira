#include <doctest.h>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QTest>
#include <QTimer>
#include <vector>

#include "NavWindow.h"
#include "app/DesktopController.h"
#include "bigscreen/GamepadInput.h"

using mira_gui::bigscreen::Nav;

namespace {

// The library-like window with a controller on it, and a game menu on the grid.
struct Library : nav_test::Library {
  QModelIndex menu_for;
  QString chosen;
  mira_gui::DesktopController controller{&window, /*in_background=*/false};

  Library() {
    // A game's menu, opened the way the library opens it: exec(), which waits for a choice.
    grid->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(grid, &QWidget::customContextMenuRequested, &window, [this](const QPoint& at) {
      menu_for = grid->indexAt(at);
      QMenu menu;
      menu.addAction("Play");
      menu.addAction("Game settings…");
      QTimer::singleShot(3000, &menu, &QMenu::close);  // a test that can't pick still ends
      if (QAction* picked = menu.exec(grid->viewport()->mapToGlobal(at))) chosen = picked->text();
    });
  }

  mira_gui::bigscreen::GamepadInput* Input() {
    return controller.findChild<mira_gui::bigscreen::GamepadInput*>();
  }

  void Press(Nav nav) {
    emit Input() -> Pressed(nav);
    QApplication::processEvents();
  }

  // Presses one after another from a repeating timer, as the controller's poll sends them.
  void PressInTurn(std::vector<Nav> navs) {
    QTimer poll;
    size_t next = 0;
    QObject::connect(&poll, &QTimer::timeout, &poll, [&] {
      if (next == navs.size()) return poll.stop();
      emit Input() -> Pressed(navs[next++]);
    });
    poll.start(50);
    CHECK(QTest::qWaitFor([&] { return !poll.isActive(); }, 5000));
    QApplication::processEvents();
  }
};

}  // namespace

TEST_CASE("A controller moves around a desktop window by where things are") {
  Library library;
  library.search->setFocus();

  // Down from the search box lands on a game in the grid under it.
  library.Press(Nav::Down);
  CHECK(library.grid->hasFocus());
  const QModelIndex first = library.grid->currentIndex();
  CHECK(first.isValid());

  // Right goes through the games, then on to the button beside the grid.
  library.Press(Nav::Right);
  CHECK(library.grid->currentIndex() != first);
  for (int i = 0; i < 6 && !library.settings->hasFocus(); ++i) library.Press(Nav::Right);
  CHECK(library.settings->hasFocus());

  // Back into the grid, and down out of it to the buttons under it.
  library.Press(Nav::Left);
  CHECK(library.grid->hasFocus());
  for (int i = 0; i < 6 && library.grid->hasFocus(); ++i) library.Press(Nav::Down);
  CHECK((library.play->hasFocus() || library.remove->hasFocus()));
}

TEST_CASE("A controller's buttons do what the window's keys do") {
  Library library;

  SUBCASE("A presses the focused button") {
    library.remove->setFocus();
    library.Press(Nav::Accept);
    CHECK(QTest::qWaitFor([&] { return library.removes == 1; }));
  }

  SUBCASE("B is Esc, also from the search box") {
    library.play->setFocus();
    library.Press(Nav::Back);
    library.search->setFocus();
    library.Press(Nav::Back);
    CHECK(library.escapes == 2);
  }

  SUBCASE("Y goes to the search box") {
    library.play->setFocus();
    library.Press(Nav::Search);
    CHECK(library.search->hasFocus());
  }

  SUBCASE("X opens the menu of the game in focus, and the stick and A pick from it") {
    library.grid->setFocus();
    library.grid->setCurrentRow(2);
    library.PressInTurn({Nav::Action, Nav::Down, Nav::Down, Nav::Accept});
    CHECK(library.menu_for == library.grid->currentIndex());
    CHECK(library.chosen == "Game settings…");
  }
}

TEST_CASE("Guide opens big screen") {
  Library library;
  int opened = 0;
  QObject::connect(&library.controller, &mira_gui::DesktopController::OpenBigScreen,
                   &library.window, [&] { ++opened; });
  library.Press(Nav::Guide);
  emit library.Input()->Released(Nav::Guide);
  QApplication::processEvents();
  CHECK(opened == 1);
}

TEST_CASE("Holding Guide, as to turn a controller off, doesn't open big screen") {
  Library library;
  int opened = 0;
  QObject::connect(&library.controller, &mira_gui::DesktopController::OpenBigScreen,
                   &library.window, [&] { ++opened; });
  library.Press(Nav::Guide);
  QTest::qWait(1700);
  emit library.Input()->Released(Nav::Guide);
  QApplication::processEvents();
  CHECK(opened == 0);
}

TEST_CASE("A controller leaves the desktop window alone while big screen is open") {
  Library library;
  library.controller.SetBigScreenOpen(true);
  library.play->setFocus();
  library.Press(Nav::Back);
  library.Press(Nav::Up);
  CHECK(library.escapes == 0);
  CHECK(library.play->hasFocus());
}
