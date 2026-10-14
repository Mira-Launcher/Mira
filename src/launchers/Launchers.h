#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Command.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "model/Types.h"
#include "store/GameStore.h"

// Store launchers with no Linux client (Battle.net, Ubisoft Connect, EA
// app), Microsoft 365 and Adobe Creative Cloud: each is installed once into its own prefix, and
// the games or apps it installs are imported as Mira games sharing that
// prefix and launched through it.
namespace mira::launchers {

// One installer a launcher's install downloads and runs.
struct Setup {
  std::string url;
  std::string file;               // saved under Mira's downloads folder
  std::vector<std::string> args;  // "{downloads}" is the downloads folder as Wine sees it
  std::string done;  // relative to drive_c, there once it finished; empty: the launcher's exe
  // How far along it is, 0-100, when the installer says so somewhere other than its output. Given
  // the prefix and the time the installer started.
  std::function<std::optional<int>(const std::filesystem::path&, std::filesystem::file_time_type)>
      progress;
  // Bytes the installer has put on disk so far: sampled for the speed in the progress line.
  std::function<std::uint64_t(const std::filesystem::path&)> bytes{};
  // (done, total) bytes of the downloads the installer has started, for a bar while it says
  // nothing.
  std::function<std::optional<std::pair<std::uint64_t, std::uint64_t>>(
      const std::filesystem::path&)>
      download{};
  // The installer's Wine session ends with it: what it left running in the prefix (Office's
  // Click-to-Run service and Wine server) is stopped, since a later launch of an app waits on that
  // session instead of starting.
  bool end_session = false;
  // It waits on the user once done (Autodesk's "Run Installer" dialog): finished as soon as `done`
  // says so, and its session is ended.
  bool stops_when_done = false;
  // Why it failed, once it has, for an installer that doesn't exit when it fails (Autodesk's).
  std::function<std::optional<std::string>()> failed{};
};

struct Launcher {
  std::string id;  // also the imported games' source
  std::string name;
  std::string umu_store;            // umu's STORE, so protonfixes apply; empty if umu has none
  std::string exe;                  // relative to drive_c
  std::vector<Setup> setups;        // run in order; none: the last winetricks verb installs it
  std::vector<std::string> tricks;  // winetricks verbs run before the installer
  bool interactive = false;         // the installer needs clicking through
  std::string kind = "games";  // what it brings: "games", or "apps" (Microsoft 365, Creative Cloud)
  std::map<std::string, std::string> env;
  // Registers mshtml, which prefixes are made without: its installer shows pages through it.
  bool needs_mshtml = false;
  // Stopping it ends everything in its prefix: closed, Creative Cloud only goes to its tray, and
  // its services stay.
  bool stop_ends_session = false;
  // A Windows app of the Apps source (Fusion, AutoCAD), which installs from an installer the user
  // brings when `installer` names it: what to download and from where.
  bool app = false;
  std::string installer;
  // Autodesk's product code in its Create Installer's file name (Create_Installer_ACD_2027_...):
  // that installer builds an install image, which then installs the app.
  std::string autodesk_code;
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
// Runs Office's installer in Microsoft 365's prefix so exactly `apps` are installed, then imports
// them.
Result<void> SetOfficeApps(config::Config& config, store::GameStore& games, api::EventBus& events,
                           std::vector<std::string> apps);
// Keeps `apps` for AddOfficeApps to add once Microsoft 365's running install ends. False when it
// isn't installing, so they can be added now.
bool QueueOfficeApps(const std::vector<std::string>& apps);
// Forgets the queued apps, e.g. after Microsoft 365's install failed.
void DropQueuedOfficeApps();
// Adds `apps` and any queued ones to the installed apps, one installer run per batch, until none
// are left. Stops at the first failure, dropping what's queued.
Result<void> AddOfficeApps(config::Config& config, store::GameStore& games, api::EventBus& events,
                           std::vector<std::string> apps);

// False if this launcher is already installing.
bool BeginInstall(const Launcher& launcher);
std::string InstallState(const Launcher& launcher);  // idle, running, finished, failed
// `on_progress` gets the install's progress, 0..1, when the installer reports it. `installer` is
// the user's own installer, for an app that needs one.
Result<model::Game> Install(config::Config& config, store::GameStore& games,
                            const Launcher& launcher,
                            const std::function<void(double)>& on_progress = {},
                            const std::string& installer = {});

// Runs the launcher, asking it to start `game` unless it is the launcher's
// own entry. `action` is launch or install.
Result<Command> BuildCommand(config::Config& config, const store::GameStore& games,
                             const model::Game& game, std::string_view action = "launch");

// install_path as Wine shows it in a process's command line, for tracking
// the game once the launcher starts it ("c:/program files (x86)/hearthstone").
std::string WindowsDir(const model::Game& game);

// What tracks the game while it runs: WindowsDir, or for an app (Word, Photoshop)
// its own exe, since Microsoft 365's share one folder.
std::string TrackedPath(const model::Game& game);

// Stops everything running in `prefix`, in the background: asks, then insists.
void EndPrefixSession(const std::string& prefix);

// Host path of a Windows path inside `prefix`.
std::filesystem::path HostPath(const std::filesystem::path& prefix, std::string_view windows_path);

// Values of every subkey of `parent` in a Wine .reg file, by subkey name.
using RegValues = std::map<std::string, std::string>;
std::map<std::string, RegValues> ReadRegSubkeys(const std::filesystem::path& reg_file,
                                                std::string_view parent);

Result<library::ImportSummary> Import(config::Config& config, store::GameStore& games,
                                      api::EventBus& events, const Launcher& launcher);

// Imports from every installed launcher; used by scans.
library::ImportSummary ImportAll(config::Config& config, store::GameStore& games,
                                 api::EventBus& events);

}  // namespace mira::launchers
