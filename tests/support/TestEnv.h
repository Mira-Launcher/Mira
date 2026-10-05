#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "api/EventBus.h"
#include "config/Config.h"
#include "store/GameStore.h"

// Shared test setup. New tests should start from TestEnv rather than
// building their own config and store.
namespace mira::test {

// A fresh, empty directory under $TMPDIR/mira-tests.
std::filesystem::path TempDir(std::string_view name);

// Creates `path` (and its parents) with `content`.
void Touch(const std::filesystem::path& path, std::string_view content = "", bool executable = false);

// Keeps `config` off the network and out of the user's own folders: no
// metadata, umu or Steam Web API lookups, every folder Mira uses (library,
// prefixes, store installs, runners, menu entries, Steam, Lutris) next to the
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

// A temp state directory with a loaded, isolated Config, GameStore and EventBus.
struct TestEnv {
  explicit TestEnv(std::string_view name);

  std::filesystem::path dir;
  config::Config config;
  store::GameStore games;
  api::EventBus events;
};

}  // namespace mira::test
