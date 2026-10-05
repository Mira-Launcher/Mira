#include <doctest.h>

#include <filesystem>
#include <fstream>

#include <toml.hpp>

#include "core/TomlJson.h"
#include "proc/Stats.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

namespace {
proc::SessionRecord MakeRecord(const std::string& game_id, std::int64_t started_at) {
  proc::SessionRecord record;
  record.game_id = game_id;
  record.started_at = started_at;
  record.finished = true;
  record.ended_at = started_at + 10;
  record.duration_seconds = 10;
  record.exit_code = 0;
  return record;
}
}  // namespace

TEST_CASE("AppendSession creates stats.toml and appends further sessions to it") {
  const fs::path stats_file = TempDir("stats-append") / "stats.toml";

  REQUIRE(proc::AppendSession(stats_file, MakeRecord("celeste", 100)).has_value());
  REQUIRE(proc::AppendSession(stats_file, MakeRecord("celeste", 200)).has_value());
  REQUIRE(proc::AppendSession(stats_file, MakeRecord("peak", 300)).has_value());

  toml::parse_result parsed = toml::parse_file(stats_file.string());
  REQUIRE(parsed);
  const auto whole = tomljson::ToJson(parsed.table());
  REQUIRE(whole.contains("session"));
  REQUIRE(whole["session"].is_array());
  CHECK(whole["session"].size() == 3);
  CHECK(whole["session"][0]["game_id"] == "celeste");
  CHECK(whole["session"][2]["game_id"] == "peak");
}

TEST_CASE("AppendSession moves a corrupt stats.toml aside and starts fresh") {
  const fs::path dir = TempDir("stats-corrupt");
  const fs::path stats_file = dir / "stats.toml";
  std::ofstream(stats_file) << "not valid toml {{{";

  REQUIRE(proc::AppendSession(stats_file, MakeRecord("celeste", 100)).has_value());

  CHECK(fs::exists(dir / "stats.toml.bad"));
  toml::parse_result parsed = toml::parse_file(stats_file.string());
  REQUIRE(parsed);
  const auto whole = tomljson::ToJson(parsed.table());
  REQUIRE(whole["session"].size() == 1);
}
