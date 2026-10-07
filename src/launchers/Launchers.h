#pragma once

#include <filesystem>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Command.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/GameStore.h"

// Store launchers with no Linux client (Battle.net, Ubisoft Connect, EA
// app), and Microsoft 365: each is installed once into its own prefix, and
// the games or apps it installs are imported as Mira games sharing that
// prefix and launched through it.
namespace mira::launchers {

// One installer a launcher's install downloads and runs.
struct Setup {
  std::string url;
  std::string file;               // saved under Mira's downloads folder
  std::vector<std::string> args;  // "{downloads}" is the downloads folder as Wine sees it
  std::string done;               // relative to drive_c, there once it finished; empty: the launcher's exe
  // How far along it is, 0-100, when the installer says so somewhere other than its output. Given the
  // prefix and the time the installer started.
  std::function<std::optional<int>(const std::filesystem::path&, std::filesystem::file_time_type)> progress;
  // Bytes the installer has put on disk so far: sampled for the speed in the progress line.
  std::function<std::uint64_t(const std::filesystem::path&)> bytes{};
  // (done, total) bytes of the downloads the installer has started, for a bar while it says nothing.
  std::function<std::optional<std::pair<std::uint64_t, std::uint64_t>>(const std::filesystem::path&)> download{};
  // The installer's Wine session ends with it: what it left running in the prefix (Office's Click-to-Run service and
  // Wine server) is stopped, since a later launch of an app waits on that session instead of starting.
  bool end_session = false;
};

struct Launcher {
  std::string id;    // also the imported games' source
  std::string name;
  std::string umu_store;  // umu's STORE, so protonfixes apply; empty if umu has none
  std::string exe;   // relative to drive_c
  std::vector<Setup> setups;              // run in order; none: the last winetricks verb installs it
  std::vector<std::string> tricks;        // winetricks verbs run before the installer
  bool interactive = false;               // the installer needs clicking through
  std::map<std::string, std::string> env;
};

const std::vector<Launcher>& All();
const Launcher* Find(std::string_view id);

// `text` (whole lines of an installer's output) without the lines that only look like trouble.
std::string WithoutNoise(std::string_view text);
std::string GameId(const Launcher& launcher);  // "launcher-<id>"

// The launcher a game belongs to: its own entry, or one it imported.
const Launcher* ForGame(const model::Game& game);

bool Installed(const store::GameStore& games, const Launcher& launcher);

// Microsoft 365's apps (office::App refs) installed in its prefix.
std::vector<std::string> InstalledOfficeApps(const model::Game& host);
// Runs Office's installer in Microsoft 365's prefix so exactly `apps` are installed, then imports them.
Result<void> SetOfficeApps(config::Config& config, store::GameStore& games, api::EventBus& events,
                           std::vector<std::string> apps);

// False if this launcher is already installing.
bool BeginInstall(const Launcher& launcher);
std::string InstallState(const Launcher& launcher);  // idle, running, finished, failed
// `on_progress` gets the install's progress, 0..1, when the installer reports it.
Result<model::Game> Install(config::Config& config, store::GameStore& games, const Launcher& launcher,
                            const std::function<void(double)>& on_progress = {});

// Runs the launcher, asking it to start `game` unless it is the launcher's
// own entry. `action` is launch or install.
Result<Command> BuildCommand(config::Config& config, const store::GameStore& games, const model::Game& game,
                             std::string_view action = "launch");

// install_path as Wine shows it in a process's command line, for tracking
// the game once the launcher starts it ("c:/program files (x86)/hearthstone").
std::string WindowsDir(const model::Game& game);

// What tracks the game while it runs: WindowsDir, or for a Microsoft 365 app
// its own exe, since all of them share one folder.
std::string TrackedPath(const model::Game& game);

// Host path of a Windows path inside `prefix`.
std::filesystem::path HostPath(const std::filesystem::path& prefix, std::string_view windows_path);

// Values of every subkey of `parent` in a Wine .reg file, by subkey name.
using RegValues = std::map<std::string, std::string>;
std::map<std::string, RegValues> ReadRegSubkeys(const std::filesystem::path& reg_file, std::string_view parent);

struct ImportSummary {
  int added = 0;
  int updated = 0;
  std::vector<model::Game> added_games;
};

Result<ImportSummary> Import(config::Config& config, store::GameStore& games, api::EventBus& events,
                             const Launcher& launcher);

// Imports from every installed launcher; used by scans.
ImportSummary ImportAll(config::Config& config, store::GameStore& games, api::EventBus& events);

}  // namespace mira::launchers
