#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "store/GameStore.h"

// Shared test setup. New tests should start from TestEnv rather than
// building their own config and store.
namespace mira::test {

// $TMPDIR/mira-tests-<pid>, where every test's files go, so two runs at once don't share them.
// main() points HOME and the XDG folders inside it, and removes it after a passing run.
const std::filesystem::path& TestsRoot();

// A fresh, empty directory under TestsRoot().
std::filesystem::path TempDir(std::string_view name);

// Whether this machine has `what` a test needs. Without it the test is reported as skipped, or
// fails where MIRA_TESTS_REQUIRE_ALL is set (CI, which is set up to run every test).
bool Has(bool available, std::string_view what);

// Whether `systemd-run --user --scope` works here, which game launches need.
bool SystemdUserSession();

// A fresh directory on another disk than TestsRoot() (/dev/shm/mira-tests-<pid>/<name>), for what
// Mira does differently per disk; nullopt, reported through Has, where /dev/shm isn't one.
std::optional<std::filesystem::path> OtherDiskDir(std::string_view name);

// The per-run roots on each disk, for main() to clear.
std::vector<std::filesystem::path> TestsRoots();

// Creates `path` (and its parents) with `content`.
void Touch(const std::filesystem::path& path, std::string_view content = "",
           bool executable = false);

// Polls `done` until it holds or `timeout` passes: what a test waits on instead of a fixed sleep.
bool WaitUntil(const std::function<bool()>& done,
               std::chrono::milliseconds timeout = std::chrono::seconds(10));

// Keeps `config` off the network and out of the user's own folders: no
// metadata, umu or Steam Web API lookups, every folder Mira uses (library,
// prefixes, store installs, runners, menu entries, Steam, Lutris, Heroic) next to the
// settings file, and Windows games run natively instead of through the
// machine's Wine. A test of metadata turns `metadata.enabled` back on itself.
void Isolate(config::Config& config);

// Puts `dir` first on PATH for its lifetime, so stand-ins there shadow real tools.
class PathPrepend {
 public:
  explicit PathPrepend(const std::filesystem::path& dir);
  ~PathPrepend();
  PathPrepend(const PathPrepend&) = delete;
  PathPrepend& operator=(const PathPrepend&) = delete;

 private:
  std::string old_;
};

// Sets an environment variable (unsets it for null) for its lifetime, restoring it afterwards.
class EnvVar {
 public:
  EnvVar(const char* name, const char* value);
  ~EnvVar();
  EnvVar(const EnvVar&) = delete;
  EnvVar& operator=(const EnvVar&) = delete;

 private:
  const char* name_;
  std::optional<std::string> old_;
};

// A temp state directory with a loaded, isolated Config, GameStore and EventBus.
struct TestEnv {
  explicit TestEnv(std::string_view name);

  std::filesystem::path dir;
  config::Config config;
  store::GameStore games;
  api::EventBus events;
};

}  // namespace mira::test
