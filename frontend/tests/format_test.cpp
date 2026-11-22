#include "components/Format.h"

#include <doctest.h>

using namespace mira_gui;

TEST_CASE("Counts read as words, singular only for one") {
  CHECK(Counted(1, "game") == "1 game");
  CHECK(Counted(0, "game") == "0 games");
  CHECK(Counted(3, "file") == "3 files");
  CHECK(Counted(2, "entry", "entries") == "2 entries");
  CHECK(Counted(1, "entry", "entries") == "1 entry");
  CHECK(ShownOf(43, 45, 0) == "43 of 45 games");
  CHECK(ShownOf(2, 0, 2) == "2 of 2 apps");
  CHECK(ShownOf(45, 45, 2) == "45 of 47 games and apps");
  CHECK(JoinAnd({"Celeste", "Hollow Knight", "Balatro"}) == "Celeste, Hollow Knight and Balatro");
}
