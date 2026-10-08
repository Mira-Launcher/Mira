#include <doctest.h>

#include <filesystem>

#include "proc/Session.h"
#include "store/GameStore.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

TEST_CASE("a session record round-trips through mira.db and is uncounted until mirad counts it") {
  const fs::path dir = TempDir("session-roundtrip");
  store::GameStore games(dir / "mira.db");
  games.Load();
  model::Game game;
  game.id = "celeste";
  REQUIRE(games.Upsert(game).has_value());

  proc::SessionRecord record;
  record.game_id = "celeste";
  record.wrapper_pid = 1234;
  record.game_pid = 1235;
  record.started_at = 1700000000;
  REQUIRE(proc::WriteSessionRecord(games.File(), record).has_value());
  auto read = proc::ReadSessionRecord(games.File(), "celeste", 1700000000);
  REQUIRE(read.has_value());
  CHECK_FALSE(read->finished);
  CHECK_FALSE(read->post_exit_code.has_value());

  record.finished = true;
  record.ended_at = 1700000042;
  record.duration_seconds = 42;
  record.exit_code = 3;
  record.post_exit_code = 0;
  REQUIRE(proc::WriteSessionRecord(games.File(), record).has_value());
  read = proc::ReadSessionRecord(games.File(), "celeste", 1700000000);
  REQUIRE(read.has_value());
  CHECK(read->wrapper_pid == 1234);
  CHECK(read->game_pid == 1235);
  CHECK(read->finished);
  CHECK(read->duration_seconds == 42);
  CHECK(read->exit_code == 3);
  CHECK(read->post_exit_code == 0);
  CHECK(proc::UncountedSessions(games.File()).size() == 1);

  REQUIRE(games.FinishSession({.game_id = "celeste", .started_at = 1700000000, .duration_seconds = 42},
                              [](model::Game&) {})
              .has_value());
  CHECK(proc::UncountedSessions(games.File()).empty());
  CHECK_FALSE(proc::ReadSessionRecord(games.File(), "celeste", 1).has_value());
}
