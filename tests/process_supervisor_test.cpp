#include <doctest.h>
#include <signal.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <thread>

#include "api/EventBus.h"
#include "core/Command.h"
#include "proc/ProcessIndex.h"
#include "proc/ProcessSupervisor.h"
#include "proc/Session.h"
#include "runner/Exec.h"
#include "store/GameStore.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

namespace {
// Waits up to `timeout` for `predicate()` to become true, polling rather
// than sleeping the whole timeout. These tests spawn real subprocesses, so
// exact timing isn't guaranteed.
bool WaitFor(std::function<bool()> predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return predicate();
}
}  // namespace

TEST_CASE("ProcessSupervisor::Launch runs post_script once the game exits cleanly") {
  const fs::path state = TempDir("proc-post-script-state");
  const fs::path marker = state / "post-ran";

  store::GameStore games(state / "mira.db");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events, /*stop_timeout_s=*/2);

  model::Game game;
  game.id = "quick-exit";
  REQUIRE(games.Upsert(game).has_value());

  Command command;
  command.argv = {"sh", "-c", "exit 0"};
  const std::string post_script = "touch " + marker.string();
  REQUIRE(supervisor.Launch(game, command, post_script).has_value());

  CHECK(WaitFor([&] { return fs::exists(marker); }, std::chrono::seconds(5)));
  CHECK(WaitFor([&] { return !supervisor.IsRunning("quick-exit"); }, std::chrono::seconds(5)));

  auto stored = games.Find("quick-exit");
  REQUIRE(stored.has_value());
  CHECK(stored->last_error.empty());  // clean exit, not a crash
}

TEST_CASE("ProcessSupervisor reports a crash with a hint and a fix that opens the game's log") {
  const fs::path state = TempDir("proc-crash-state");
  store::GameStore games(state / "mira.db");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events, /*stop_timeout_s=*/2);

  model::Game game;
  game.id = "crasher";
  game.platform = model::Platform::Windows;
  REQUIRE(games.Upsert(game).has_value());

  Command command;
  command.argv = {"sh", "-c", "kill -SEGV $$"};
  REQUIRE(supervisor.Launch(game, command, "").has_value());

  nlohmann::json crashed;
  CHECK(WaitFor(
      [&] {
        for (const model::Event& e : events.Since(0)) {
          if (e.type == "game.state" && e.payload.value("state", "") == "crashed") crashed = e.payload;
        }
        return !crashed.is_null();
      },
      std::chrono::seconds(5)));
  CHECK(crashed.value("signal", 0) == SIGSEGV);
  CHECK(crashed.value("error", "").find("segmentation fault") != std::string::npos);
  CHECK_FALSE(crashed.value("hint", "").empty());
  CHECK(crashed["fix"] == nlohmann::json{{"kind", "game"}, {"target", "crasher"}, {"step", "log"}});
  CHECK(games.Find("crasher")->last_error == crashed.value("error", ""));
}

TEST_CASE("ProcessSupervisor treats a non-zero exit as an ordinary quit") {
  const fs::path state = TempDir("proc-nonzero-state");
  store::GameStore games(state / "mira.db");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events, /*stop_timeout_s=*/2);

  model::Game game;
  game.id = "quitter";
  game.last_error = "Crashed after 4 minutes: invalid memory access (segmentation fault)";
  REQUIRE(games.Upsert(game).has_value());

  Command command;
  command.argv = {"sh", "-c", "exit 241"};
  REQUIRE(supervisor.Launch(game, command, "").has_value());

  nlohmann::json exited;
  CHECK(WaitFor(
      [&] {
        for (const model::Event& e : events.Since(0)) {
          if (e.type == "game.state" && e.payload.value("state", "") == "exited") exited = e.payload;
        }
        return !exited.is_null();
      },
      std::chrono::seconds(5)));
  CHECK(exited.value("exit_code", 0) == 241);
  CHECK(games.Find("quitter")->last_error.empty());  // an earlier crash doesn't stick
}

TEST_CASE("Quitting mirad leaves a running game alone and doesn't wait for it") {
  const fs::path state = TempDir("proc-quit-state");
  store::GameStore games(state / "mira.db");
  games.Load();
  api::EventBus events;
  auto supervisor = std::make_unique<proc::ProcessSupervisor>(games, events);

  model::Game game;
  game.id = "still-playing";
  REQUIRE(games.Upsert(game).has_value());
  Command command;
  command.argv = {"sleep", "30"};
  command.env["WINEPREFIX"] = (state / "pfx").string();  // to find the game again below
  REQUIRE(supervisor->Launch(game, command).has_value());
  REQUIRE(WaitFor([&] { return !proc::FindPrefixProcesses((state / "pfx").string()).empty(); },
                  std::chrono::seconds(5)));

  const auto started = std::chrono::steady_clock::now();
  supervisor.reset();
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
  const auto left = proc::FindPrefixProcesses((state / "pfx").string());
  CHECK_FALSE(left.empty());

  for (pid_t pid : left) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
  }
}

TEST_CASE("ProcessSupervisor::Launch rejects a duplicate launch while one is already running") {
  const fs::path state = TempDir("proc-duplicate-state");
  store::GameStore games(state / "mira.db");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);

  model::Game game;
  game.id = "long-running";
  REQUIRE(games.Upsert(game).has_value());

  Command command;
  command.argv = {"sleep", "5"};
  REQUIRE(supervisor.Launch(game, command).has_value());

  const auto second = supervisor.Launch(game, command);
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error().code == "already_running");

  REQUIRE(supervisor.Stop("long-running").has_value());
  CHECK(WaitFor([&] { return !supervisor.IsRunning("long-running"); }, std::chrono::seconds(5)));
}

TEST_CASE("ProcessSupervisor::TrackSteamLaunch detects and tracks Steam's reaper for the AppId, "
         "records playtime, and runs post_script on exit") {
  const fs::path state = TempDir("proc-steam-track-state");
  const fs::path marker = state / "post-ran";

  store::GameStore games(state / "mira.db");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);

  model::Game game;
  game.id = "steam-game";
  REQUIRE(games.Upsert(game).has_value());

  // Simulate Steam's own wrapper: a process named reaper with "SteamLaunch
  // AppId=<id>" in its command line. The test is its parent, so it reaps it
  // (a zombie still passes kill(pid, 0)). "; true" keeps sh from exec'ing
  // sleep, which would replace that command line.
  const fs::path fake_reaper = state / "reaper";
  fs::create_symlink("/bin/sh", fake_reaper);
  Command fake_steam_process;
  fake_steam_process.argv = {fake_reaper.string(), "-c", "sleep 2; true", "SteamLaunch", "AppId=999999"};
  auto pid = runner::SpawnDetached(fake_steam_process);
  REQUIRE(pid.has_value());
  std::thread reaper([pid = *pid] { ::waitpid(pid, nullptr, 0); });
  reaper.detach();

  const std::string post_script = "touch " + marker.string();
  REQUIRE(supervisor.TrackSteamLaunch(game, "999999", post_script).has_value());

  CHECK(WaitFor([&] { return supervisor.IsRunning("steam-game"); }, std::chrono::seconds(5)));
  CHECK(WaitFor(
      [&] {
        auto so_far = events.Since(0);
        return std::ranges::any_of(so_far, [](const model::Event& e) {
          return e.type == "game.state" && e.payload.value("state", "") == "running";
        });
      },
      std::chrono::seconds(5)));

  CHECK(WaitFor([&] { return fs::exists(marker); }, std::chrono::seconds(10)));
  CHECK(WaitFor([&] { return !supervisor.IsRunning("steam-game"); }, std::chrono::seconds(5)));

  auto stored = games.Find("steam-game");
  REQUIRE(stored.has_value());
  CHECK(stored->play_seconds >= 1);
}

TEST_CASE("ProcessSupervisor::Stop refuses a not-yet-confirmed TrackSteamLaunch instead of "
         "signalling pid 0") {
  const fs::path state = TempDir("proc-steam-unconfirmed-state");
  store::GameStore games(state / "mira.db");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);

  model::Game game;
  game.id = "never-shows-up";
  REQUIRE(games.Upsert(game).has_value());

  // No process with this appid actually exists -- TrackSteamLaunch reserves
  // the slot immediately (pid 0 sentinel) and only replaces it once
  // detection succeeds, so Stop() called in that window must not try to
  // kill(-0, ...) (every process in mirad's own process group).
  REQUIRE(supervisor.TrackSteamLaunch(game, "111111111").has_value());
  CHECK(supervisor.IsRunning("never-shows-up"));

  const auto stopped = supervisor.Stop("never-shows-up");
  REQUIRE_FALSE(stopped.has_value());
  CHECK(stopped.error().code == "not_yet_confirmed");
}

// FindPrefixProcesses exists because Stop()'s process-group signal reached
// 1 of 16 processes on a real Proton launch (umu-run, wineserver, etc. all
// setsid()). These cover what the prefix match must and must not catch.

TEST_CASE("SpawnDetached reports a program that can't start instead of a pid") {
  const fs::path dir = TempDir("spawn-cannot-start");
  Command missing;
  missing.argv = {(dir / "not-there").string()};
  auto not_found = runner::SpawnDetached(missing);
  REQUIRE_FALSE(not_found.has_value());
  CHECK(not_found.error().code == "exec_failed");

  test::Touch(dir / "not-executable", "#!/bin/sh\n", /*executable=*/false);
  Command denied;
  denied.argv = {(dir / "not-executable").string()};
  CHECK_FALSE(runner::SpawnDetached(denied).has_value());

  Command gone_folder;
  gone_folder.argv = {"true"};
  gone_folder.cwd = dir / "gone";
  CHECK_FALSE(runner::SpawnDetached(gone_folder).has_value());

  Command fine;
  fine.argv = {"true"};
  auto pid = runner::SpawnDetached(fine);
  REQUIRE(pid.has_value());
  ::waitpid(*pid, nullptr, 0);
}

TEST_CASE("FindPrefixProcesses finds a process that left its process group") {
  const fs::path prefix = TempDir("proc-prefix-escaped");

  // setsid(), like wineserver does, the env var is all that's left tying
  // this process to the game.
  Command command;
  command.argv = {"sh", "-c", "setsid sleep 30 & sleep 30"};
  command.env["WINEPREFIX"] = prefix.string();
  auto pid = runner::SpawnDetached(command);
  REQUIRE(pid.has_value());

  CHECK(WaitFor([&] { return proc::FindPrefixProcesses(prefix.string()).size() >= 2; },
                std::chrono::seconds(5)));

  for (pid_t found : proc::FindPrefixProcesses(prefix.string())) ::kill(found, SIGKILL);
  ::waitpid(*pid, nullptr, 0);
  CHECK(WaitFor([&] { return proc::FindPrefixProcesses(prefix.string()).empty(); },
                std::chrono::seconds(5)));
}

TEST_CASE("FindPrefixProcesses does not match a game whose prefix is a string prefix") {
  // "…/animal" must not stop "…/animal-well". A substring search over
  // /proc/<pid>/environ would, which is why the match is per entry with an
  // explicit separator check.
  const fs::path base = TempDir("proc-prefix-neighbour");
  const fs::path narrow = base / "animal";
  const fs::path wide = base / "animal-well";
  fs::create_directories(narrow);
  fs::create_directories(wide);

  Command command;
  command.argv = {"sh", "-c", "sleep 30"};
  command.env["WINEPREFIX"] = wide.string();
  auto pid = runner::SpawnDetached(command);
  REQUIRE(pid.has_value());

  CHECK(WaitFor([&] { return !proc::FindPrefixProcesses(wide.string()).empty(); },
                std::chrono::seconds(5)));
  CHECK(proc::FindPrefixProcesses(narrow.string()).empty());

  ::kill(-*pid, SIGKILL);
  ::kill(*pid, SIGKILL);
  ::waitpid(*pid, nullptr, 0);
}

TEST_CASE("Reconcile archives a finished session a previous mirad never got to see") {
  const fs::path state = TempDir("proc-reconcile-finished-state");

  store::GameStore games(state / "mira.db");
  games.Load();
  model::Game game;
  game.id = "celeste";
  game.play_seconds = 100;
  REQUIRE(games.Upsert(game).has_value());

  proc::SessionRecord record;
  record.game_id = "celeste";
  record.wrapper_pid = 999999;  // long dead / never existed, irrelevant since the record is already finished
  record.started_at = 1700000000;
  record.finished = true;
  record.ended_at = 1700000042;
  record.duration_seconds = 42;
  record.exit_code = 0;
  REQUIRE(proc::WriteSessionRecord(games.File(), record).has_value());

  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);
  supervisor.Reconcile();

  CHECK(proc::UncountedSessions(games.File()).empty());
  auto stored = games.Find("celeste");
  REQUIRE(stored.has_value());
  CHECK(stored->play_seconds == 142);  // 100 already banked + 42 from the reconciled session

  // mira-run writing the same record again (a late final write) doesn't make it count twice.
  REQUIRE(proc::WriteSessionRecord(games.File(), record).has_value());
  supervisor.Reconcile();
  CHECK(games.Find("celeste")->play_seconds == 142);
}

TEST_CASE("A Windows game whose log has Wine's unhandled-exception report counts as crashed") {
  const fs::path state = TempDir("proc-wine-crash-state");
  store::GameStore games(state / "mira.db");
  games.Load();
  model::Game game;
  game.id = "splodey";
  game.platform = model::Platform::Windows;
  REQUIRE(games.Upsert(game).has_value());

  const auto finish = [&](int exit_code, std::int64_t started_at) {
    proc::SessionRecord record;
    record.game_id = "splodey";
    record.started_at = started_at;
    record.finished = true;
    record.duration_seconds = 250;
    record.exit_code = exit_code;
    REQUIRE(proc::WriteSessionRecord(games.File(), record).has_value());
  };
  test::Touch(proc::GameLogPath(state, "splodey"),
              "Proton: Executable is a unix path, launching with 'umu.exe'.\n"
              "wine: Unhandled page fault on read access to 0000000000000000 at address 0000000140001000 "
              "(thread 0024), starting debugger...\n"
              "[mira-run] game exited: exit_code=5 signal=0 duration=250s\n");

  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);
  finish(5, 1700000000);
  supervisor.Reconcile();
  CHECK(games.Find("splodey")->last_error == "Crashed after 4 minutes with an unhandled page fault on read access");

  // A helper process crashing while the game itself quits cleanly isn't the game crashing.
  finish(0, 1700000500);
  supervisor.Reconcile();
  CHECK(games.Find("splodey")->last_error.empty());
}

TEST_CASE("Reconcile closes out a session as incomplete when its wrapper is gone too") {
  const fs::path state = TempDir("proc-reconcile-dead-state");

  store::GameStore games(state / "mira.db");
  games.Load();
  model::Game game;
  game.id = "celeste";
  REQUIRE(games.Upsert(game).has_value());

  proc::SessionRecord record;
  record.game_id = "celeste";
  record.wrapper_pid = 999999;  // not a real pid on any sane machine
  record.started_at = 1700000000;
  record.finished = false;  // still "in flight" as far as the file says
  REQUIRE(proc::WriteSessionRecord(games.File(), record).has_value());

  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);
  supervisor.Reconcile();

  CHECK(proc::UncountedSessions(games.File()).empty());
  auto stored = games.Find("celeste");
  REQUIRE(stored.has_value());
  CHECK(stored->last_error.find("restarted") != std::string::npos);
}

TEST_CASE("Reconcile re-adopts a session whose wrapper is still alive, tracking it as running") {
  const fs::path state = TempDir("proc-reconcile-live-state");

  store::GameStore games(state / "mira.db");
  games.Load();
  model::Game game;
  game.id = "celeste";
  REQUIRE(games.Upsert(game).has_value());

  // Standing in for a real mira-run: a real process, so kill(pid, 0) has
  // something genuine to answer about. Reconcile can never actually
  // waitpid() this (it isn't this process's child), which is exactly the
  // case it exists to handle.
  Command command;
  command.argv = {"sh", "-c", "sleep 30"};
  auto pid = runner::SpawnDetached(command);
  REQUIRE(pid.has_value());

  proc::SessionRecord record;
  record.game_id = "celeste";
  record.wrapper_pid = *pid;
  record.started_at = model::NowSeconds();
  record.finished = false;
  REQUIRE(proc::WriteSessionRecord(games.File(), record).has_value());

  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);
  supervisor.Reconcile();

  CHECK(supervisor.IsRunning("celeste"));

  ::kill(*pid, SIGKILL);
  ::waitpid(*pid, nullptr, 0);  // reap it ourselves; Reconcile's watcher only polls kill(pid, 0)
  CHECK(WaitFor([&] { return !supervisor.IsRunning("celeste"); }, std::chrono::seconds(5)));
}

TEST_CASE("Reconcile imports sessions/*.toml from an older Mira and drops a corrupt one") {
  const fs::path state = TempDir("proc-reconcile-import-state");
  const fs::path sessions_dir = state / "sessions";
  fs::create_directories(sessions_dir);
  std::ofstream(sessions_dir / "broken.toml") << "not valid toml {{{";
  std::ofstream(sessions_dir / "celeste-1700000000.toml")
      << "game_id = 'celeste'\nwrapper_pid = 1\nstarted_at = 1700000000\nfinished = true\nduration_seconds = 30\n";

  store::GameStore games(state / "mira.db");
  games.Load();
  model::Game game;
  game.id = "celeste";
  REQUIRE(games.Upsert(game).has_value());
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);
  supervisor.Reconcile();  // must not throw or hang

  CHECK(games.Find("celeste")->play_seconds == 30);
  CHECK_FALSE(fs::exists(sessions_dir));
}

TEST_CASE("FindPrefixProcesses matches umu's rewritten WINEPREFIX and an empty one matches nothing") {
  // umu rewrites WINEPREFIX to "<data_dir>/pfx/" before the game runs, so
  // the separator case is the normal case, not an edge one.
  const fs::path data_dir = TempDir("proc-prefix-pfx");
  const fs::path pfx = data_dir / "pfx";
  fs::create_directories(pfx);

  Command command;
  command.argv = {"sh", "-c", "sleep 30"};
  command.env["WINEPREFIX"] = pfx.string() + "/";
  auto pid = runner::SpawnDetached(command);
  REQUIRE(pid.has_value());

  CHECK(WaitFor([&] { return !proc::FindPrefixProcesses(data_dir.string()).empty(); },
                std::chrono::seconds(5)));
  // A game with no prefix at all (a native one) must never sweep up every
  // process on the machine.
  CHECK(proc::FindPrefixProcesses("").empty());

  ::kill(-*pid, SIGKILL);
  ::kill(*pid, SIGKILL);
  ::waitpid(*pid, nullptr, 0);
}

TEST_CASE("SteamLaunchAppId reads Steam's reaper wrapper and nothing else") {
  const auto parse = [](const std::string& argv0, const std::string& rest) {
    std::istringstream in(rest);
    return proc::SteamLaunchAppId(argv0, in);
  };
  const std::string args = std::string("SteamLaunch\0AppId=2225070\0--\0/x/_v2-entry-point\0AppId=1\0", 55);
  CHECK(parse("/home/u/.local/share/Steam/ubuntu12_32/reaper", args) == "2225070");
  CHECK(parse("/usr/bin/python3", args).empty());
  CHECK(parse("/home/u/.local/share/Steam/ubuntu12_32/reaper", std::string("--\0AppId=5\0", 11)).empty());
}
