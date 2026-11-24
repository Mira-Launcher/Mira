#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

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

// The formats Mira can run silently; anything else is kUnknown.
InstallerFormat DetectInstallerFormat(const std::filesystem::path& file);

// The user asked for what's in `folder` to be added, as with a download from links: its installer
// runs on its own. Lasts until mirad restarts.
void RequestAutoInstall(const std::filesystem::path& folder);
bool IsAutoInstallRequested(const std::filesystem::path& folder);
std::string_view ToString(InstallerFormat format);

struct InstallerInfo {
  std::filesystem::path path;
  std::uintmax_t size_bytes = 0;
  InstallerFormat format = InstallerFormat::kUnknown;
  bool silent = false;      // a known format Mira can run unattended
  std::string silent_args;  // empty unless silent
  // Library roots whose disk can hold the prefix, when the installer sits outside them (a disc or
  // a mounted image, often full or read-only); the first is the default. Empty otherwise.
  struct Target {
    std::filesystem::path root;
    std::uintmax_t free_bytes = 0;
  };
  std::vector<Target> targets;
};

// The installer a game not set up points at (its exe_path).
Result<InstallerInfo> DescribeInstaller(const config::Config& config, const model::Game& game);

struct InstallProgress {
  std::string state;  // queued, running, finished, failed
  std::string mode;   // silent or interactive, once running
  std::int64_t started_at = 0;
  std::int64_t finished_at = 0;
  std::string error;
  std::uintmax_t bytes_written = 0;   // growth of the install folder and new drive_c folders
  std::int64_t estimated_bytes = -1;  // EstimatedInstallBytes of the game's metadata, at the start
};

struct InstalledApp {
  std::filesystem::path dir;
  std::string exe_path;  // relative to dir; empty when no program was found
};

// install.detect_dirs' folders, the user's AppData/Local where per-user installers put a program,
// and folders right in C:\: taken before an installer runs, for NewPrograms and NewInstall.
std::set<std::filesystem::path> ProgramFolders(const config::Config& config,
                                               const std::filesystem::path& prefix);
// The program in each folder added to `prefix` since `before` (from ProgramFolders), leaving out
// runtimes' folders (.NET's), helpers (detect.deny_name_patterns), updaters and uninstallers.
std::vector<InstalledApp> NewPrograms(const config::Config& config,
                                      const std::filesystem::path& prefix,
                                      const std::set<std::filesystem::path>& before);
// The game an installer added: the biggest of NewPrograms, rather than a launcher or a runtime
// installed beside it.
std::optional<InstalledApp> NewInstall(const config::Config& config,
                                       const std::filesystem::path& prefix,
                                       const std::set<std::filesystem::path>& before);
// What a program installed in `prefix` is called: GOG's info file, the name its installer
// registered for the uninstaller, its folder's name, then the exe's product name, skipping generic
// ones ("Game", "bin"). Empty when all are.
std::string InstalledName(const std::filesystem::path& prefix, const InstalledApp& app);

// The latest install for `id` since mirad started, if any.
std::optional<InstallProgress> Progress(const std::string& id);
// Whether `id`'s installer is running now, writing to the game's log.
bool Installing(const std::string& id);

enum class InstallMode {
  kSilentOnly,   // known formats only, unattended (scanner)
  kAuto,         // silent when the format is known, otherwise shown for the user to click through
  kInteractive,  // always shown
};

// Queues `id` for an install; false if one is already queued or running.
bool BeginInstall(const std::string& id);

// Runs the installer of a game that isn't set up in its prefix, one at a time, writing its
// output to the game's log. A quiet install targets C:\Games\<name> in the prefix. Then finds the
// game exe there, among what it wrote to install_path, or in a new drive_c folder.
// `installer` overrides exe_path and is saved as the game's installer.
// Saves the result: to confirm on success, last_error on failure.
// `target`, a library root, makes a prefix not yet in Mira's prefix folders on that root's disk
// instead, for an installer on a disc or mounted image.
Result<model::Game> Install(config::Config& config, store::GameStore& games, const std::string& id,
                            InstallMode mode, const std::optional<std::filesystem::path>& installer,
                            const std::optional<std::filesystem::path>& target = std::nullopt);

// Install, reported: game.install.started, game.updated, then game.install.finished or
// .failed (with the error's hint and fix). On success it updates the menu entries and,
// with `fetches`, fetches the game's art and store info again. After BeginInstall.
Result<void> RunInstall(config::Config& config, store::GameStore& games, api::EventBus& events,
                        metadata::FetchQueue* fetches, const std::string& id, InstallMode mode,
                        const std::optional<std::filesystem::path>& installer,
                        const std::optional<std::filesystem::path>& target = std::nullopt);

// Publishes game.installer_leftover when the game's installer folder (installer_dir) is still
// on disk inside a library root, so a client can offer to delete it.
void AnnounceInstallerLeftover(const config::Config& config, api::EventBus& events,
                               const model::Game& game);

// Deletes the game's installer_dir, only inside a library root and never when it holds a game of
// `library` (itself included) or its prefix.
Result<void> DeleteInstallerFolder(const config::Config& config, const model::Game& game,
                                   const std::vector<model::Game>& library);

// Whether a scan runs this game's installer on its own (quietly, downloads.auto_install).
bool AutoInstalls(const config::Config& config, const model::Game& game);

// Points `game` at the folder its installer put it in, remembering the installer's folder
// in installer_dir, and renames it by InstalledName while it still has its automatic name.
void AdoptInstallFolder(model::Game& game, const std::string& install_path,
                        const std::string& exe_path = {}, const std::string& prefix = {});

}  // namespace mira::library
