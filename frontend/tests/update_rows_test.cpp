#include <doctest.h>

#include "../updates/UpdateRows.h"

using namespace mira_gui;

TEST_CASE("GroupUpdates orders by kind, leaves Mira out, and merges a store's tool and helper") {
  const std::vector<UpdateInfo> updates = {
      {.kind = "mira", .id = "mira", .name = "Mira", .installed = "1", .latest = "2"},
      {.kind = "tool", .id = "gog", .name = "gogdl", .installed = "1", .latest = "2"},
      {.kind = "tool", .id = "gog", .name = "comet", .installed = "", .latest = "3"},
      {.kind = "emulator",
       .id = "duck",
       .name = "DuckStation",
       .installed = "latest",
       .latest = "latest"},
      {.kind = "runner",
       .id = "proton:a",
       .name = "GE-Proton",
       .installed = "a",
       .latest = "b",
       .release = "b"},
      {.kind = "mod_manager", .id = "mo2", .name = "MO2", .installed = "1", .latest = "2"},
  };
  const auto groups = GroupUpdates(updates);
  REQUIRE(groups.size() == 4);
  CHECK(groups[0].rows[0].kind == "runner");
  CHECK(groups[0].rows[0].release == "b");
  CHECK(groups[1].rows[0].detail == "A newer build of latest");
  CHECK(groups[2].rows[0].kind == "mod_manager");
  REQUIRE(groups[3].rows.size() == 1);  // one setup updates both
  CHECK(groups[3].rows[0].name == "gogdl and comet");
  CHECK(groups[3].rows[0].detail == "1 → 2  ·  comet Adds 3");
}

TEST_CASE("MiraEntry leaves out a build of the user's own") {
  CHECK_FALSE(MiraEntry({}));
  CHECK(MiraEntry({{.kind = "mira", .install = "manual"}}));
  CHECK_FALSE(MiraEntry({{.kind = "mira", .install = "none"}}));
}
