#include <doctest.h>

#include "setup/SetupFlow.h"

using mira_gui::setup::Choices;
using mira_gui::setup::Flow;
using mira_gui::setup::Screen;
using mira_gui::setup::ScreenKind;
using mira_gui::setup::SourcesOn;

TEST_CASE("Set up Mira asks only about what was picked, and skipped picks bring no pages") {
  Choices both;
  both.stores = {"gog", "steam", "epic"};
  CHECK(Flow(both) == QStringList{"welcome", "use", "found", "stores", "apps", "sign:steam", "sign:epic",
                                  "sign:gog", "office", "look", "sidebar", "done"});

  Choices apps_only;
  apps_only.games = false;
  CHECK(Flow(apps_only) == QStringList{"welcome", "use", "apps", "office", "look", "sidebar", "done"});

  Choices skipped = both;
  skipped.stores_skipped = true;
  skipped.apps_skipped = true;
  CHECK(Flow(skipped) == QStringList{"welcome", "use", "found", "stores", "apps", "look", "sidebar", "done"});

  Choices neither;
  neither.games = false;
  neither.apps = false;
  CHECK(Flow(neither) == QStringList{"welcome", "use", "look", "sidebar", "done"});

  Choices no_office = both;
  no_office.office = false;
  CHECK_FALSE(Flow(no_office).contains("office"));
}

TEST_CASE("Only the sources picked in setup stay on") {
  Choices picks;
  picks.stores = {"epic", "gog"};
  picks.found = {"steam"};
  CHECK(SourcesOn(picks) == QStringList{"local", "steam", "epic", "gog", "office"});

  Choices skipped = picks;
  skipped.stores_skipped = true;
  skipped.apps_skipped = true;
  CHECK(SourcesOn(skipped) == QStringList{"local", "steam"});

  Choices apps_only = picks;
  apps_only.games = false;
  CHECK(SourcesOn(apps_only) == QStringList{"local", "office"});
}

TEST_CASE("A Steam Deck is told by its panel and a TV by its size") {
  CHECK(ScreenKind({1280, 800}, {150, 94}) == Screen::SteamDeck);
  CHECK(ScreenKind({800, 1280}, {94, 150}) == Screen::SteamDeck);
  CHECK(ScreenKind({3840, 2160}, {1210, 680}) == Screen::Tv);  // 55 inches
  CHECK(ScreenKind({2560, 1440}, {597, 336}) == Screen::Desktop);  // 27 inches
  CHECK(ScreenKind({1920, 1080}, {}) == Screen::Desktop);  // a display that doesn't say
}
