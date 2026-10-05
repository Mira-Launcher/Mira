#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "proc/Session.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

TEST_CASE("WriteSessionRecord then ReadSessionRecord round-trips every field") {
  const fs::path dir = TempDir("session-roundtrip");
  const fs::path path = proc::SessionFilePath(dir, "celeste", 1700000000);

  proc::SessionRecord record;
  record.game_id = "celeste";
  record.wrapper_pid = 1234;
  record.game_pid = 1235;
  record.started_at = 1700000000;
  record.finished = true;
  record.ended_at = 1700000042;
  record.duration_seconds = 42;
  record.exit_code = 0;
  record.signal = 0;
  record.post_exit_code = 0;
  record.post_timed_out = false;
  record.incomplete = false;

  REQUIRE(proc::WriteSessionRecord(path, record).has_value());
  const auto read = proc::ReadSessionRecord(path);
  REQUIRE(read.has_value());
  CHECK(read->game_id == "celeste");
  CHECK(read->wrapper_pid == 1234);
  CHECK(read->game_pid == 1235);
  CHECK(read->started_at == 1700000000);
  CHECK(read->finished == true);
  CHECK(read->ended_at == 1700000042);
  CHECK(read->duration_seconds == 42);
  CHECK(read->exit_code == 0);
  CHECK(read->post_exit_code.has_value());
  CHECK(*read->post_exit_code == 0);
  CHECK(read->incomplete == false);
}

TEST_CASE("WriteSessionRecord omits finished-only fields for an in-flight session") {
  const fs::path dir = TempDir("session-inflight");
  const fs::path path = proc::SessionFilePath(dir, "celeste", 1700000000);

  proc::SessionRecord record;
  record.game_id = "celeste";
  record.wrapper_pid = 1234;
  record.game_pid = 1235;
  record.started_at = 1700000000;
  record.finished = false;

  REQUIRE(proc::WriteSessionRecord(path, record).has_value());
  const auto read = proc::ReadSessionRecord(path);
  REQUIRE(read.has_value());
  CHECK(read->finished == false);
  CHECK_FALSE(read->post_exit_code.has_value());
}

TEST_CASE("ReadSessionRecord fails cleanly (not throws) on a missing or corrupt file") {
  const fs::path dir = TempDir("session-corrupt");

  const auto missing = proc::ReadSessionRecord(dir / "does-not-exist.toml");
  CHECK_FALSE(missing.has_value());

  const fs::path corrupt = dir / "corrupt.toml";
  std::ofstream(corrupt) << "this is not valid toml {{{";
  const auto read_corrupt = proc::ReadSessionRecord(corrupt);
  CHECK_FALSE(read_corrupt.has_value());
}

TEST_CASE("WriteSessionRecord creates the sessions directory if missing") {
  const fs::path dir = TempDir("session-mkdir-parent") / "nested" / "sessions";
  const fs::path path = proc::SessionFilePath(dir, "celeste", 1700000000);

  proc::SessionRecord record;
  record.game_id = "celeste";
  record.started_at = 1700000000;

  REQUIRE(proc::WriteSessionRecord(path, record).has_value());
  CHECK(fs::exists(path));
}
