#include "proc/Cgroup.h"

#include <doctest.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include "core/Command.h"
#include "model/Types.h"
#include "proc/Session.h"
#include "runner/Exec.h"
#include "store/GameStore.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

// mira-run in its own scope as mirad starts it, running `script` as game "cgroup-test" with its
// database in `state`; nullopt, reported through Has, where this machine can't run it.
std::optional<Command> MiraRun(const fs::path& state, const std::vector<std::string>& flags,
                               const std::string& script) {
  if (!test::Has(test::SystemdUserSession(), "systemd user session")) return std::nullopt;
  const fs::path db = state / "mira.db";
  store::GameStore games(db);  // mira-run's session rows reference a game
  games.Load();
  model::Game game;
  game.id = "cgroup-test";
  REQUIRE(games.Upsert(game).has_value());
  return test::MiraRunCommand(game.id, db, flags, {"sh", "-c", script});
}

std::vector<std::string> Lines(const fs::path& file) {
  std::vector<std::string> lines;
  std::ifstream in(file);
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

}  // namespace

TEST_CASE("cgroup::Of gives this process a cgroup v2 path that exists") {
  const auto group = proc::cgroup::Of(::getpid());
  REQUIRE(group.has_value());
  CHECK(group->string().starts_with("/sys/fs/cgroup/"));
  CHECK(fs::exists(*group));
}

TEST_CASE("cgroup::WaitEmpty on a group that stays populated sleeps instead of spinning") {
  // This process's own group stays populated while the test runs.
  const auto group = proc::cgroup::Of(::getpid());
  REQUIRE(group.has_value());
  if (!test::Has(fs::exists(*group / "cgroup.events"), "a non-root cgroup")) return;
  timespec before{}, after{};
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &before);
  const auto started = std::chrono::steady_clock::now();
  CHECK_FALSE(proc::cgroup::WaitEmpty(*group, std::chrono::milliseconds(1500)));
  CHECK(std::chrono::steady_clock::now() - started >= std::chrono::seconds(1));
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &after);
  const double cpu_s =
      static_cast<double>(after.tv_sec - before.tv_sec) + (after.tv_nsec - before.tv_nsec) / 1e9;
  CHECK(cpu_s < 0.1);
}

TEST_CASE("mira-run keeps its session open while the game's background child runs") {
  const fs::path state = TempDir("cgroup-session-state");
  const auto cmd = MiraRun(state, {}, "sleep 2 & exit 0");
  if (!cmd) return;
  auto ran = runner::RunAndWait(*cmd);
  REQUIRE(ran.has_value());
  const auto sessions = proc::UncountedSessions(state / "mira.db");
  REQUIRE(sessions.size() == 1);
  CHECK(sessions[0].duration_seconds >= 2);
}

TEST_CASE("mira-run keeps GameMode on for a process the game hands off to, until it exits") {
  const fs::path state = TempDir("cgroup-gamemode");
  // Stands in for gdbus, keeping GameMode's registered pids in a file.
  const fs::path clients = state / "clients";
  test::Touch(clients);
  test::Touch(state / "bin" / "gdbus",
              std::format(R"sh(#!/bin/sh
method=""
while [ $# -gt 1 ]; do [ "$1" = --method ] && method=$2; shift; done
case $method in
  *.RegisterGame) echo "$1" >> '{0}' ;;
  *.UnregisterGame) grep -vx "$1" '{0}' > '{0}.new'; mv '{0}.new' '{0}' ;;
esac
echo "(0,)"
)sh",
                          clients.string()),
              true);
  const test::PathPrepend path(state / "bin");
  // The game starts its successor and exits, as a launcher or self-updater does. The successor
  // runs until `running` is removed.
  const fs::path successor = state / "successor";
  const fs::path running = state / "running";
  test::Touch(running);
  const auto cmd = MiraRun(state, {"--gamemode"},
                           std::format("(while [ -e '{}' ]; do sleep 0.1; done) & echo $! > '{}'",
                                       running.string(), successor.string()));
  if (!cmd) return;
  auto ran = std::async(std::launch::async, [&] { return runner::RunAndWait(*cmd); });

  CHECK(test::WaitUntil([&] {
    const auto pid = Lines(successor);
    return !pid.empty() && std::ranges::contains(Lines(clients), pid.front());
  }));
  fs::remove(running);
  REQUIRE(ran.get().has_value());
  CHECK(Lines(clients).empty());
}
