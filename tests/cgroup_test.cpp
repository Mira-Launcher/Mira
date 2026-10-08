#include <doctest.h>
#include <unistd.h>

#include <filesystem>

#include "core/Command.h"
#include "proc/Cgroup.h"
#include "proc/Session.h"
#include "runner/Exec.h"
#include "store/GameStore.h"
#include "model/Types.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

namespace {
bool SystemdUserAvailable() {
  Command probe;
  probe.argv = {"systemd-run", "--user", "--scope", "--quiet", "--collect", "--", "true"};
  auto ran = runner::RunAndWait(probe);
  return ran.has_value() && ran->exit_code == 0;
}
}  // namespace

TEST_CASE("cgroup::Of gives this process a cgroup v2 path that exists") {
  const auto group = proc::cgroup::Of(::getpid());
  REQUIRE(group.has_value());
  CHECK(group->string().starts_with("/sys/fs/cgroup/"));
  CHECK(fs::exists(*group));
}

TEST_CASE("mira-run keeps its session open while the game's background child runs") {
  if (!SystemdUserAvailable()) {
    MESSAGE("no systemd user session; skipped");
    return;
  }
  const fs::path mira_run = fs::read_symlink("/proc/self/exe").parent_path() / "mira-run";
  if (!fs::exists(mira_run)) {
    MESSAGE("mira-run isn't next to the test binary; skipped");
    return;
  }
  const fs::path state = TempDir("cgroup-session-state");
  const fs::path db = state / "mira.db";
  store::GameStore games(db);  // mira-run's session rows reference a game
  games.Load();
  model::Game game;
  game.id = "cgroup-test";
  REQUIRE(games.Upsert(game).has_value());

  Command cmd;
  cmd.argv = {"systemd-run", "--user", "--scope", "--quiet", "--collect", "--", mira_run.string(),
              "--game-id", "cgroup-test", "--database", db.string(), "--log-file", (state / "game.log").string(),
              "--", "sh", "-c", "sleep 2 & exit 0"};
  auto ran = runner::RunAndWait(cmd);
  REQUIRE(ran.has_value());
  const auto sessions = proc::UncountedSessions(db);
  REQUIRE(sessions.size() == 1);
  CHECK(sessions[0].duration_seconds >= 2);
}
