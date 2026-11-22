#include <doctest.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>

#include "NavWindow.h"
#include "components/SheetCard.h"

using namespace mira_gui;

namespace {

QToolButton* CloseButton(SheetCard& card) {
  for (auto* button : card.findChildren<QToolButton*>()) {
    if (button->toolTip() == "Close") return button;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("A sheet keeps its close button at the header's end and its error over its buttons") {
  nav_test::App();
  SheetCard card;
  card.SetHeading("Install Celeste?");
  bool closed = false;
  card.SetClose("Close", [&closed] { closed = true; });
  // Added after the close button, as a page adds its own controls once the card is built.
  auto* search = new QPushButton("Search", &card);
  card.AddHeaderWidget(search);
  QHBoxLayout* buttons = card.ButtonRow();
  buttons->addWidget(new QPushButton("Install", &card));
  QLabel* error = card.ErrorLine();
  CHECK(error == card.ErrorLine());
  error->setText("Couldn't read the installer");
  error->show();

  card.resize(560, 300);
  card.show();
  card.layout()->activate();

  QToolButton* close = CloseButton(card);
  REQUIRE(close != nullptr);
  CHECK(search->mapTo(&card, QPoint()).x() < close->mapTo(&card, QPoint()).x());
  CHECK(error->mapTo(&card, QPoint()).y() < buttons->parentWidget()->mapTo(&card, QPoint()).y());

  close->click();
  CHECK(closed);
}
