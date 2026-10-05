#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::api {
class EventBus;
}
namespace mira::metadata {
class FetchQueue;
}

namespace mira::library {

enum class InstallerFormat { kUnknown, kInnoSetup, kNsis, kMsi };

InstallerFormat DetectInstallerFormat(const std::filesystem::path& file);
std::string_view ToString(InstallerFormat format);

struct InstallerInfo {
  std::filesystem::path path;
  std::uintmax_t size_bytes = 0;
  InstallerFormat format = InstallerFormat::kUnknown;
  bool silent = false;       // a known format Mira can run unattended
  std::string silent_args;  // empty unless silent
};

// The installer a needs_install game points at (its exe_path).
Result<InstallerInfo> DescribeInstaller(const config::Config& config, const model::Game& game);

struct InstallProgress {
  std::string state;  // queued, running, finished, failed
  std::string mode;   // silent or interactive, once running
  std::int64_t started_at = 0;
  std::int64_t finished_at = 0;
  std::string error;
  std::uintmax_t bytes_written = 0;  // growth of the install folder and new drive_c folders
};

// Top-level folders under install.detect_dirs in `prefix`'s drive_c: taken
// before a launch, so NewInstall can tell what the run installed.
std::set<std::filesystem::path> InstallFolders(const config::Config& config, const std::filesystem::path& prefix);

struct InstalledApp {
  std::filesystem::path dir;
  std::string exe_path;  // relative to dir; empty when no program was found
};

// A folder added to `prefix` since `before`, preferring one with a program in it.
std::optional<InstalledApp> NewInstall(const config::Config& config, const std::filesystem::path& prefix,
                                       const std::set<std::filesystem::path>& before);

// The latest install for `id` since mirad started, if any.
std::optional<InstallProgress> Progress(const std::string& id);

enum class InstallMode {
  kSilentOnly,   // known formats only, unattended (scanner)
  kAuto,         // silent when the format is known, otherwise shown for the user to click through
  kInteractive,  // always shown
};

// Queues `id` for an install; false if one is already queued or running.
bool BeginInstall(const std::string& id);

// Runs a needs_install (or broken) game's installer in its prefix, one at a
// time, then finds the game exe in install_path or a new drive_c folder.
// `installer` overrides exe_path and is saved as the game's installer.
// Saves the result: Ready on success, last_error on failure.
Result<model::Game> Install(config::Config& config, store::GameStore& games, const std::string& id,
                            InstallMode mode, const std::optional<std::filesystem::path>& installer);

// Install, reported: game.install.started, game.updated, then game.install.finished or
// .failed (with the error's hint and fix). On success it updates the menu entries and,
// with `fetches`, fetches the game's art and store info again. After BeginInstall.
Result<void> RunInstall(config::Config& config, store::GameStore& games, api::EventBus& events,
                        metadata::FetchQueue* fetches, const std::string& id, InstallMode mode,
                        const std::optional<std::filesystem::path>& installer);

// Publishes game.installer_leftover when the game's installer folder (installer_dir) is still
// on disk, so a client can offer to delete it.
void AnnounceInstallerLeftover(api::EventBus& events, const model::Game& game);

// Deletes the game's installer_dir, only inside a library root and never when it holds the
// game itself or its prefix.
Result<void> DeleteInstallerFolder(const config::Config& config, const model::Game& game);

// Whether a scan runs this game's installer on its own (quietly, scan.auto_run_installers).
bool AutoInstalls(const config::Config& config, const model::Game& game);

// Points `game` at the folder its installer put it in, remembering the installer's folder
// in installer_dir, and renames it after that folder while it still has its automatic name.
void AdoptInstallFolder(model::Game& game, const std::string& install_path);

}  // namespace mira::library
