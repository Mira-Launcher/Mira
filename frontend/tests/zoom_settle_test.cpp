#include <doctest.h>

#include <QWidget>

#include "NavWindow.h"
#include "library/ArtworkStore.h"
#include "library/ZoomSettle.h"

using namespace mira_gui;

TEST_CASE("Covers are scaled quickly only while some view's zoom moves") {
  nav_test::App();
  ArtworkStore artwork;
  auto* library = new QWidget;
  auto* picker = new QWidget;
  library->show();
  picker->show();
  auto* library_zoom = new ZoomSettle(library, &artwork, 200, [] {});
  auto* picker_zoom = new ZoomSettle(picker, &artwork, 200, [] {});

  for (const int width : {210, 220, 230, 240}) library_zoom->Step(width);
  picker_zoom->Step(180);
  CHECK(artwork.QuickScaling());

  delete library;  // its zoom ends with it
  CHECK(artwork.QuickScaling());
  delete picker;
  CHECK_FALSE(artwork.QuickScaling());

  // A view off screen only changes width.
  QWidget hidden;
  ZoomSettle hidden_zoom(&hidden, &artwork, 200, [] {});
  hidden_zoom.Step(260);
  CHECK_FALSE(artwork.QuickScaling());
}
