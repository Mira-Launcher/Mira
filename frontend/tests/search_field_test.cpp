#include <doctest.h>

#include <QTest>
#include <QToolButton>

#include "NavWindow.h"
#include "components/SearchField.h"

using namespace mira_gui;

namespace {

// The clear button QLineEdit draws for the trailing action, while it's shown.
QToolButton* ShownClearButton(QLineEdit& field) {
  for (auto* button : field.findChildren<QToolButton*>()) {
    if (button->isVisible() && button->defaultAction() != nullptr &&
        button->defaultAction()->toolTip() == "Clear the search") {
      return button;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("A search field offers to clear only while it has text, and clearing empties it") {
  nav_test::App();
  SearchField field("Search games");
  field.resize(240, 32);
  field.show();
  CHECK(ShownClearButton(field) == nullptr);

  QTest::keyClicks(&field, "Celeste");
  QToolButton* clear = ShownClearButton(field);
  REQUIRE(clear != nullptr);
  QTest::mouseClick(clear, Qt::LeftButton);
  CHECK(field.text().isEmpty());
  CHECK(ShownClearButton(field) == nullptr);
}
