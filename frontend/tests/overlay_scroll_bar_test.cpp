#include <doctest.h>

#include <QApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include "NavWindow.h"
#include "components/OverlayScrollBar.h"

using namespace mira_gui;

namespace {

void Send(QWidget* widget, QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
  QMouseEvent event(type, at, widget->mapToGlobal(at), Qt::LeftButton, buttons, Qt::NoModifier);
  QApplication::sendEvent(widget, &event);
}

QScrollArea* TallArea(QWidget* parent) {
  auto* area = new QScrollArea(parent);
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);
  for (int i = 0; i < 60; ++i) layout->addWidget(new QLabel("Celeste", page));
  area->setWidget(page);
  area->setWidgetResizable(true);
  area->resize(300, 200);
  return area;
}

OverlayScrollBar* OverlayFor(QScrollArea* area) {
  for (auto* overlay : area->findChildren<OverlayScrollBar*>()) {
    if (overlay->Bar() == area->verticalScrollBar()) return overlay;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("A scroll area's bar shows while it scrolls and drags the page") {
  nav_test::App();
  SetUpOverlayScrollBars();
  QWidget window;
  QScrollArea* area = TallArea(&window);
  window.resize(320, 220);
  window.show();
  QApplication::processEvents();

  QScrollBar* bar = area->verticalScrollBar();
  REQUIRE(bar->maximum() > 0);
  OverlayScrollBar* overlay = OverlayFor(area);
  REQUIRE(overlay != nullptr);

  bar->setValue(bar->maximum() / 4);
  CHECK(overlay->Shown());

  // Dragging the thumb past the end scrolls to the end.
  const QPointF grab = overlay->Thumb().center();
  Send(overlay, QEvent::MouseButtonPress, grab, Qt::LeftButton);
  Send(overlay, QEvent::MouseMove, grab + QPointF(0, area->height()), Qt::LeftButton);
  Send(overlay, QEvent::MouseButtonRelease, grab + QPointF(0, area->height()), Qt::NoButton);
  CHECK(bar->value() == bar->maximum());
}

TEST_CASE("A scroll area that turns its bar off shows no overlay for it") {
  nav_test::App();
  SetUpOverlayScrollBars();
  QWidget window;
  QScrollArea* area = TallArea(&window);
  area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  window.resize(320, 220);
  window.show();
  QApplication::processEvents();
  REQUIRE(area->verticalScrollBar()->maximum() > 0);
  area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
  OverlayScrollBar* overlay = OverlayFor(area);
  CHECK((overlay == nullptr || !overlay->Shown()));
}
