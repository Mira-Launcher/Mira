#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "migrate/Legacy.h"
#include "proc/Session.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

// Temporary, like migrate/Legacy.h: delete with it.
TEST_CASE("files from Mira 0.13 are moved into the databases once") {
  const fs::path dir = TempDir("legacy-import");
  std::ofstream(dir / "games.toml") << "[[game]]\nid = 'celeste'\nname = 'Celeste'\nstatus = 'ready'\n"
                                       "tags = ['hidden', 'favorite']\n";
  fs::create_directories(dir / "metadata");
  std::ofstream(dir / "metadata" / "celeste.json") << R"({"description": "hi"})";
  std::ofstream(dir / "metadata" / "broken.json") << "{ not json";
  fs::create_directories(dir / "sessions");
  std::ofstream(dir / "sessions" / "celeste-1700000000.toml")
      << "game_id = 'celeste'\nwrapper_pid = 1\nstarted_at = 1700000000\nfinished = true\nduration_seconds = 30\n";
  std::ofstream(dir / "frontend.toml") << "theme = 'dark'\nwindow_width = 900\n";

  store::GameStore games(dir / "mira.db");
  games.Load();
  migrate::ImportLegacyFiles(games);

  REQUIRE(games.Find("celeste").has_value());
  CHECK(games.Find("celeste")->tags == std::vector<std::string>{"hidden", "favorite"});
  CHECK(fs::exists(dir / "games.toml.migrated"));
  CHECK(games.Metadata().Read("celeste").value("description", "") == "hi");
  CHECK_FALSE(fs::exists(dir / "metadata"));
  CHECK(proc::UncountedSessions(games.File()).size() == 1);
  CHECK_FALSE(fs::exists(dir / "sessions"));
  CHECK(games.UiState().value("window_width", 0) == 900);
  std::ifstream frontend(dir / "frontend.toml");
  const std::string text{std::istreambuf_iterator<char>(frontend), {}};
  CHECK(text.find("window_width") == std::string::npos);
  CHECK(text.find("theme") != std::string::npos);
}
