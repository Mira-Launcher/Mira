#include <doctest.h>

#include "library/SourceRemoval.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("DeleteInside only deletes strictly inside a root") {
  const fs::path root = test::TempDir("delete-inside");
  test::Touch(root / "game" / "game.exe");
  test::Touch(root.parent_path() / "delete-inside-outside" / "keep");

  CHECK_FALSE(library::DeleteInside(root.string(), {root}));
  CHECK(fs::exists(root));
  CHECK_FALSE(
      library::DeleteInside((root.parent_path() / "delete-inside-outside").string(), {root}));
  CHECK(fs::exists(root.parent_path() / "delete-inside-outside" / "keep"));
  CHECK(library::DeleteInside((root / "game").string(), {root}));
  CHECK_FALSE(fs::exists(root / "game"));
}

TEST_CASE("DeleteInside leaves a folder above or beside a deeper root alone") {
  const fs::path root = test::TempDir("delete-inside-deep");
  test::Touch(root / "a" / "b" / "c" / "file");

  // The target is shorter than the root's path: nothing above the root may go.
  CHECK_FALSE(library::DeleteInside(root.string(), {root / "a" / "b" / "c"}));
  CHECK_FALSE(library::DeleteInside((root / "a").string(), {root / "a" / "b" / "c"}));
  CHECK(fs::exists(root / "a" / "b" / "c" / "file"));
}

TEST_CASE("Removing a launcher plans no deletion of its apps: their folder goes with the launcher's") {
  test::TestEnv env("remove-launcher-apps");
  const fs::path prefix = env.dir / "prefix";
  const fs::path program = prefix / "drive_c" / "Program Files" / "Microsoft Office";
  test::Touch(program / "root" / "Office16" / "WINWORD.EXE");

  model::Game launcher;
  launcher.id = "launcher-office";
  launcher.name = "Microsoft 365";
  launcher.source = "launcher";
  launcher.source_ref = "office";
  launcher.data_dir = prefix.string();
  launcher.install_path = (program / "root" / "Office16").string();
  REQUIRE(env.games.Upsert(launcher).has_value());

  model::Game word = launcher;
  word.id = "office-word";
  word.name = "Word";
  word.source = "office";
  word.source_ref = "word";
  word.exe_path = "WINWORD.EXE";
  REQUIRE(env.games.Upsert(word).has_value());

  const auto plan = library::PlanRemoval(env.config, env.games, "office");
  REQUIRE(plan.has_value());
  REQUIRE(plan->games.size() == 1);
  CHECK(plan->games.front().deletes.empty());  // not "its folder", which is the launcher's to delete

  const auto removed = library::RemoveSource(env.config, env.games, env.events, "office");
  REQUIRE(removed.has_value());
  CHECK(removed->problems.empty());  // no false "also holds Microsoft 365"
  CHECK_FALSE(fs::exists(program));
}
