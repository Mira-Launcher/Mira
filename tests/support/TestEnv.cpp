#include "support/TestEnv.h"

#include <cstdlib>
#include <fstream>
#include <thread>
#include <utility>

#include <json.hpp>

namespace mira::test {
namespace fs = std::filesystem;

fs::path TempDir(std::string_view name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

bool WaitUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

void Touch(const fs::path& path, std::string_view content, bool executable) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << content;
  if (executable) fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
}

PathPrepend::PathPrepend(const fs::path& dir)
    : old_(std::getenv("PATH") != nullptr ? std::getenv("PATH") : "") {
  setenv("PATH", (dir.string() + ":" + old_).c_str(), 1);
}

PathPrepend::~PathPrepend() { setenv("PATH", old_.c_str(), 1); }

void Isolate(config::Config& config) {
  for (const char* key : {"metadata.enabled", "metadata.steam_art_by_name", "metadata.steam_by_name",
                          "launchers.umu_lookup", "steam.import_playtime", "runner_scan_common_dirs", "tags.steam"}) {
    [[maybe_unused]] auto off = config.Set(key, false);
  }
  // Every folder Mira reads or writes, moved next to the settings file.
  const fs::path dir = config.File().parent_path();
  const std::pair<const char*, nlohmann::json> folders[] = {
      {"library_roots", nlohmann::json::array()},
      {"prefix_root", (dir / "prefixes").string()},
      {"epic.install_root", (dir / "epic").string()},
      {"gog.install_root", (dir / "gog").string()},
      {"itch.install_root", (dir / "itch").string()},
      {"amazon.install_root", (dir / "amazon").string()},
      {"humble.download_root", (dir / "humble").string()},
      {"runner_search_paths", nlohmann::json::array({(dir / "runners" / "proton").string()})},
      {"wine_search_paths", nlohmann::json::array({(dir / "runners" / "wine").string()})},
      {"desktop_entries.directory", (dir / "applications").string()},
      {"steam.root", (dir / "steam").string()},
      {"lutris.data_dir", (dir / "lutris").string()},
  };
  for (const auto& [key, value] : folders) {
    [[maybe_unused]] auto set = config.Set(key, value);
  }
  // Legendary's own sign-in and caches, which Mira reads without a setting.
  setenv("LEGENDARY_CONFIG_PATH", (dir / "legendary-config").c_str(), 1);
  // Games' shader caches.
  setenv("XDG_CACHE_HOME", (dir / "cache").c_str(), 1);
  // Windows games run natively: provisioning through the machine's Wine builds a real prefix.
  [[maybe_unused]] auto runner = config.Set("default_runner.windows", "native:native");
}

TestEnv::TestEnv(std::string_view name)
    : dir(TempDir(name)), config(dir / "settings.toml"), games(dir / "mira.db") {
  config.Load();
  games.Load();
  Isolate(config);
}

}  // namespace mira::test
