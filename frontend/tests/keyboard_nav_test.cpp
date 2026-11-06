#include <doctest.h>

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTest>
#include <vector>

#include "NavWindow.h"
#include "widgets/FocusRing.h"
#include "widgets/SpatialNav.h"

using nav_test::App;
using nav_test::Library;

TEST_CASE("The keyboard's arrows move around a desktop window by where things are") {
  Library library;
  library.search->setFocus();

  // Down from the search box lands on a game in the grid under it.
  QTest::keyClick(library.search, Qt::Key_Down);
  CHECK(library.grid->hasFocus());
  CHECK(library.grid->currentIndex().isValid());

  // Right goes through the games, then on to the button beside the grid.
  for (int i = 0; i < 6 && library.grid->hasFocus(); ++i)
    QTest::keyClick(library.grid, Qt::Key_Right);
  CHECK(library.settings->hasFocus());

  // Back into the grid, and down out of it to the buttons under it.
  QTest::keyClick(library.settings, Qt::Key_Left);
  CHECK(library.grid->hasFocus());
  for (int i = 0; i < 6 && library.grid->hasFocus(); ++i)
    QTest::keyClick(library.grid, Qt::Key_Down);
  CHECK((library.play->hasFocus() || library.remove->hasFocus()));

  library.play->setFocus();
  QTest::keyClick(library.play, Qt::Key_Right);
  CHECK(library.remove->hasFocus());
}

TEST_CASE("Enter presses the focused button") {
  Library library;
  library.remove->setFocus();
  QTest::keyClick(library.remove, Qt::Key_Return);
  CHECK(QTest::qWaitFor([&] { return library.removes == 1; }));
}

TEST_CASE("The keyboard's arrows pass over choices and dropdowns without changing them") {
  App();  // before any widget
  QWidget window;
  window.setGeometry(100, 100, 600, 400);
  // A row of theme-style choices (Qt makes only the checked one a Tab stop), a dropdown under it
  // and a page with a text field under that.
  auto* group = new QButtonGroup(&window);
  std::vector<QPushButton*> choices;
  for (int i = 0; i < 3; ++i) {
    auto* choice = new QPushButton(QString("Theme %1").arg(i), &window);
    choice->setCheckable(true);
    choice->setGeometry(10 + 110 * i, 10, 100, 60);
    group->addButton(choice);
    choices.push_back(choice);
  }
  choices[0]->setChecked(true);
  auto* size = new QComboBox(&window);
  size->addItems({"100%", "125%", "150%"});
  size->setGeometry(10, 90, 330, 30);
  auto* page = new QScrollArea(&window);
  page->setGeometry(10, 140, 400, 100);
  auto* content = new QWidget;
  auto* name = new QLineEdit("Mira", content);
  name->setGeometry(10, 10, 200, 30);
  content->resize(380, 60);
  page->setWidget(content);
  mira_gui::FocusRing::Install();
  mira_gui::spatial_nav::Install();
  window.show();
  window.activateWindow();
  REQUIRE(QTest::qWaitForWindowActive(&window));

  choices[0]->setFocus();
  QTest::keyClick(choices[0], Qt::Key_Right);
  CHECK(choices[1]->hasFocus());
  QTest::keyClick(choices[1], Qt::Key_Right);
  CHECK(choices[2]->hasFocus());
  CHECK(choices[0]->isChecked());

  QTest::keyClick(choices[2], Qt::Key_Down);
  CHECK(size->hasFocus());
  QTest::keyClick(size, Qt::Key_Down);
  CHECK(name->hasFocus());
  CHECK(size->currentText() == "100%");

  // A text field keeps the arrows for its cursor until it reaches the end.
  name->setCursorPosition(4);
  QTest::keyClick(name, Qt::Key_Left);
  CHECK(name->hasFocus());
  CHECK(name->cursorPosition() == 3);
  QTest::keyClick(name, Qt::Key_Home);
  QTest::keyClick(name, Qt::Key_Up);
  CHECK(size->hasFocus());

  // A page's scroll that took focus from a click hands it to the page's first control.
  page->setFocus(Qt::MouseFocusReason);
  QTest::keyClick(page, Qt::Key_Down);
  CHECK(name->hasFocus());
}

TEST_CASE("After a click, the first arrow starts where the user pointed") {
  Library library;
  library.search->setFocus();

  SUBCASE("A click on nothing: from that spot") {
    QTest::mouseClick(&library.window, Qt::LeftButton, {}, {130, 360});
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Up);
    CHECK(library.remove->hasFocus());
  }

  SUBCASE("A click on a control that sent focus elsewhere: onto that control") {
    QObject::connect(library.settings, &QPushButton::clicked, library.search,
                     [&] { library.search->setFocus(); });
    QTest::mouseClick(library.settings, Qt::LeftButton);
    REQUIRE(library.search->hasFocus());
    QTest::keyClick(library.search, Qt::Key_Down);
    CHECK(library.settings->hasFocus());
  }

  SUBCASE("A click on the focused control: on from it") {
    QTest::mouseClick(library.play, Qt::LeftButton);
    REQUIRE(library.play->hasFocus());
    QTest::keyClick(library.play, Qt::Key_Right);
    CHECK(library.remove->hasFocus());
  }
}

TEST_CASE("Tab goes through a window in the order things are on screen") {
  App();  // before any widget
  QWidget window;
  window.setGeometry(100, 100, 600, 400);
  // A top bar, and a sidebar whose rows are built out of order, as rows added later are.
  auto* top = new QWidget(&window);
  top->setGeometry(0, 0, 600, 40);
  auto* zoom = new QPushButton("Zoom", top);
  zoom->setGeometry(400, 5, 80, 30);
  mira_gui::spatial_nav::TabLast(top);
  auto* sidebar = new QWidget(&window);
  sidebar->setGeometry(0, 40, 150, 360);
  auto* sources = new QWidget(sidebar);
  sources->setGeometry(0, 120, 150, 100);
  auto* steam = new QPushButton("Steam", sources);
  steam->setGeometry(5, 40, 140, 30);
  auto* local = new QPushButton("Local", sources);
  local->setGeometry(5, 5, 140, 30);
  auto* library = new QPushButton("Library", sidebar);
  library->setGeometry(5, 5, 140, 30);
  auto* pinned = new QPushButton("Celeste", sidebar);
  pinned->setGeometry(5, 50, 140, 30);
  auto* play = new QPushButton("Play", &window);
  play->setGeometry(200, 60, 100, 30);
  auto* remove = new QPushButton("Remove", &window);
  remove->setGeometry(200, 300, 100, 30);
  mira_gui::spatial_nav::Install();
  window.show();
  window.activateWindow();
  REQUIRE(QTest::qWaitForWindowActive(&window));

  library->setFocus();
  std::vector<QString> order;
  for (int i = 0; i < 7; ++i) {
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
    QApplication::processEvents();
    order.push_back(static_cast<QPushButton*>(QApplication::focusWidget())->text());
  }
  CHECK(order ==
        std::vector<QString>{"Celeste", "Local", "Steam", "Play", "Remove", "Zoom", "Library"});

  // A card over the content: Tab passes what's under it.
  auto* card = new QWidget(&window);
  card->setGeometry(180, 280, 200, 80);
  card->show();
  card->raise();
  play->setFocus();
  QTest::keyClick(play, Qt::Key_Tab);
  CHECK(QTest::qWaitFor([&] { return zoom->hasFocus(); }));
}

TEST_CASE("The arrows can't reach a control under a card") {
  Library library;
  auto* card = new QWidget(&library.window);
  card->setGeometry(115, 280, 200, 60);
  card->show();
  card->raise();
  library.play->setFocus();
  QTest::keyClick(library.play, Qt::Key_Right);
  CHECK_FALSE(library.remove->hasFocus());
}

TEST_CASE("Keyboard focus shows a ring, and a mouse click hides it") {
  Library library;
  const auto ring = [&] { return library.window.findChild<QWidget*>("focus_ring"); };
  const auto ringed = [&](QWidget* widget, const QRect& rect) {
    const QWidget* shown = ring();
    return shown != nullptr && shown->isVisible() &&
           shown->geometry().contains(
               QRect(widget->mapTo(&library.window, rect.topLeft()), rect.size()));
  };

  library.play->setFocus();
  QTest::keyClick(&library.window, Qt::Key_Tab);
  CHECK(QTest::qWaitFor([&] { return ringed(library.remove, library.remove->rect()); }));

  // In the grid the game in focus is selected, and its own look marks it: one mark, not two.
  library.search->setFocus();
  QTest::keyClick(library.search, Qt::Key_Down);
  CHECK(library.grid->hasFocus());
  CHECK(library.grid->selectionModel()->isSelected(library.grid->currentIndex()));
  CHECK(QTest::qWaitFor([&] { return !ring()->isVisible(); }));

  for (int i = 0; i < 6 && library.grid->hasFocus(); ++i)
    QTest::keyClick(library.grid, Qt::Key_Right);
  CHECK(library.settings->hasFocus());
  CHECK(QTest::qWaitFor([&] { return ringed(library.settings, library.settings->rect()); }));

  // Tabbing back to a grid whose selection was cleared selects the game it was on again.
  library.grid->clearSelection();
  library.grid->setFocus(Qt::TabFocusReason);
  CHECK(QTest::qWaitFor(
      [&] { return library.grid->selectionModel()->isSelected(library.grid->currentIndex()); }));
  QTest::mouseClick(library.play, Qt::LeftButton);
  CHECK(QTest::qWaitFor([&] { return !ring()->isVisible(); }));
}
