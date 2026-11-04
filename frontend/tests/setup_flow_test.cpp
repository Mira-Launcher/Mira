#include <doctest.h>

#include <algorithm>

#include "setup/SetupFlow.h"

using mira_gui::setup::Choices;
using mira_gui::setup::Flow;
using mira_gui::setup::Screen;
using mira_gui::setup::ScreenKind;
using mira_gui::setup::SourcesOn;

namespace {

bool SignsIn(const QStringList& flow) {
  return std::ranges::any_of(flow, [](const QString& page) { return page.startsWith("sign:"); });
}

QStringList Sorted(QStringList list) {
  list.sort();
  return list;
}

}  // namespace

TEST_CASE("Set up Mira asks only about what was picked, and skipped picks bring no pages") {
  const QStringList nothing = Flow(Choices{});
  CHECK(nothing.front() == "welcome");
  CHECK(nothing.back() == "done");
  for (const char* page : {"found", "stores", "apps", "office"})
    CHECK_FALSE(nothing.contains(page));
  CHECK_FALSE(SignsIn(nothing));

  Choices both{.games = true, .apps = true};
  both.stores = {"gog", "steam", "epic"};
  const QStringList all = Flow(both);
  for (const char* page :
       {"found", "stores", "apps", "sign:steam", "sign:epic", "sign:gog", "office"}) {
    CHECK(all.contains(page));
  }
  CHECK(all.indexOf("stores") < all.indexOf("sign:gog"));  // signs in to what was just picked

  Choices apps_only{.apps = true};
  const QStringList apps = Flow(apps_only);
  CHECK(apps.contains("office"));
  CHECK_FALSE(apps.contains("stores"));

  Choices skipped = both;
  skipped.stores_skipped = true;
  skipped.apps_skipped = true;
  CHECK_FALSE(SignsIn(Flow(skipped)));
  CHECK_FALSE(Flow(skipped).contains("office"));

  Choices no_office = both;
  no_office.office = false;
  CHECK_FALSE(Flow(no_office).contains("office"));
}

TEST_CASE("Only the sources picked in setup stay on") {
  Choices picks{.games = true, .apps = true};
  picks.stores = {"epic", "gog"};
  picks.found = {"steam"};
  CHECK(Sorted(SourcesOn(picks)) == Sorted({"local", "steam", "epic", "gog", "office"}));

  Choices skipped = picks;
  skipped.stores_skipped = true;
  skipped.apps_skipped = true;
  CHECK(Sorted(SourcesOn(skipped)) == Sorted({"local", "steam"}));

  Choices apps_only = picks;
  apps_only.games = false;
  CHECK(Sorted(SourcesOn(apps_only)) == Sorted({"local", "office"}));
}

TEST_CASE("A Steam Deck is told by its panel and a TV by its size") {
  CHECK(ScreenKind({1280, 800}, {150, 94}) == Screen::SteamDeck);
  CHECK(ScreenKind({800, 1280}, {94, 150}) == Screen::SteamDeck);
  CHECK(ScreenKind({3840, 2160}, {1210, 680}) == Screen::Tv);      // 55 inches
  CHECK(ScreenKind({2560, 1440}, {597, 336}) == Screen::Desktop);  // 27 inches
  CHECK(ScreenKind({1920, 1080}, {}) == Screen::Desktop);          // a display that doesn't say
}
