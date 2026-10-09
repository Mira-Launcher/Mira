#include <doctest.h>

#include "sources/Sources.h"

using namespace mira_gui;

TEST_CASE("A saved source order keeps its own order and places sources it doesn't name by default") {
  // Saved before Local existed, with Steam moved below the stores.
  const std::vector<QString> order = OrderSources({"epic", "gog", "steam", "lutris"});
  REQUIRE(order.size() == AllSources().size());
  CHECK(order[0] == "local");
  const auto at = [&](const char* id) { return std::ranges::find(order, QString(id)) - order.begin(); };
  CHECK(at("epic") < at("gog"));
  CHECK(at("gog") < at("steam"));
  CHECK(at("steam") < at("lutris"));
  // itch follows GOG by default, so it slots in right after it.
  CHECK(at("itch") == at("gog") + 1);

  // Moved by hand, Local stays where it was put; unknown ids are dropped.
  const std::vector<QString> moved = OrderSources({"steam", "local", "nonsense"});
  CHECK(moved[0] == "steam");
  CHECK(std::ranges::find(moved, QString("local")) - moved.begin() > 0);
  CHECK(moved.size() == AllSources().size());
}
