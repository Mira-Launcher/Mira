#include "library/AutoInstall.h"

#include <signal.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include "api/EventBus.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "desktop/DesktopEntries.h"
#include "library/Detector.h"
#include "library/PrefixNaming.h"
#include "library/SourceRemoval.h"
#include "metadata/FetchQueue.h"
#include "proc/ProcessSupervisor.h"
#include "proc/Session.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

constexpr std::uintmax_t kSniffWindow = 2 * 1024 * 1024;

std::string ReadWindow(std::ifstream& in, std::uintmax_t offset, std::uintmax_t max_len) {
  in.seekg(static_cast<std::streamoff>(offset));
  std::string buf(static_cast<size_t>(max_len), '\0');
  in.read(buf.data(), static_cast<std::streamsize>(max_len));
  buf.resize(static_cast<size_t>(in.gcount()));
  return buf;
}

std::string SilentArgsFor(const config::Config& config, InstallerFormat format) {
  switch (format) {
    case InstallerFormat::kInnoSetup: {
      std::string args = config.GetString("install.inno_args");
      if (config.GetBool("install.show_progress")) {
        const auto at = args.find("/VERYSILENT");
        if (at != std::string::npos) args.replace(at, 11, "/SILENT");
      }
      return args;
    }
    case InstallerFormat::kNsis:
      return config.GetString("install.nsis_args");
    case InstallerFormat::kMsi:
      return config.GetString("install.msi_args");
    case InstallerFormat::kUnknown:
      return "";
  }
  return "";
}

}  // namespace

namespace {

// Both ends of a file, since an installer builder's mark can sit anywhere in a large installer.
struct Sniffed {
  std::string head;
  std::string tail;
  bool Has(std::string_view needle) const {
    return head.find(needle) != std::string::npos || tail.find(needle) != std::string::npos;
  }
};

std::optional<Sniffed> Sniff(const fs::path& file) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(file, ec);
  if (ec || size == 0) return std::nullopt;
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  Sniffed sniffed;
  sniffed.head = ReadWindow(in, 0, std::min<std::uintmax_t>(size, kSniffWindow));
  if (size > kSniffWindow) sniffed.tail = ReadWindow(in, size - kSniffWindow, kSniffWindow);
  return sniffed;
}

InstallerFormat SilentFormat(const Sniffed& sniffed) {
  if (sniffed.Has("Inno Setup")) return InstallerFormat::kInnoSetup;
  if (sniffed.Has("Nullsoft")) return InstallerFormat::kNsis;
  return InstallerFormat::kUnknown;
}

}  // namespace

// An .msi by its extension; an .exe by its builder's mark.
InstallerFormat DetectInstallerFormat(const fs::path& file) {
  const std::string ext = strings::ToLower(file.extension().string());
  if (ext == ".msi") return InstallerFormat::kMsi;
  if (ext != ".exe") return InstallerFormat::kUnknown;
  const std::optional<Sniffed> sniffed = Sniff(file);
  return sniffed ? SilentFormat(*sniffed) : InstallerFormat::kUnknown;
}

bool IsBuiltInstaller(const fs::path& file) {
  if (strings::ToLower(file.extension().string()) == ".msi") return true;
  const std::optional<Sniffed> sniffed = Sniff(file);
  if (!sniffed) return false;
  return SilentFormat(*sniffed) != InstallerFormat::kUnknown || sniffed->Has(".wixburn") ||
         sniffed->Has("InstallShield") || sniffed->Has("MojoSetup");
}

namespace {

struct Tracked {
  InstallProgress progress;
  fs::path install_path;
  std::uintmax_t baseline_bytes = 0;
  fs::path data_dir;
  std::vector<std::string> detect_dirs;
  std::set<fs::path> drive_c_before;
  std::chrono::steady_clock::time_point measured_at;  // when bytes_written was last walked
};

std::mutex tracked_mutex;
std::map<std::string, Tracked> tracked;
std::mutex install_run_mutex;  // one installer at a time

using paths::TreeBytes;

void Finish(const std::string& id, const std::string& error) {
  const std::lock_guard lock(tracked_mutex);
  InstallProgress& progress = tracked[id].progress;
  progress.state = error.empty() ? "finished" : "failed";
  progress.error = error;
  progress.finished_at = model::NowSeconds();
}

// Top-level folders under install.detect_dirs in the prefix's drive_c.
std::set<fs::path> InstallDirs(const std::vector<std::string>& parents, const fs::path& prefix) {
  std::set<fs::path> dirs;
  std::error_code ec;
  for (const std::string& parent : parents) {
    for (const auto& entry : fs::directory_iterator(prefix / "drive_c" / parent, ec)) {
      if (entry.is_directory(ec)) dirs.insert(entry.path());
    }
  }
  return dirs;
}

bool HoldsFiles(const fs::path& dir) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) return true;
  }
  return false;
}

// A publisher's folder ("Program Files/Ubisoft/<Game>") holds only folders; the game's own is the
// first one on the way to its executable that holds files.
fs::path GameFolder(const fs::path& dir, const fs::path& exe_rel_path) {
  fs::path folder = dir;
  for (const fs::path& part : exe_rel_path.parent_path()) {
    if (HoldsFiles(folder)) break;
    folder /= part;
  }
  return folder;
}

// Each executable under `root` with its modification time.
using ExeTimes = std::map<std::string, fs::file_time_type>;
ExeTimes Executables(const Detector& detector, const fs::path& root) {
  ExeTimes times;
  std::error_code ec;
  for (const model::Candidate& candidate : detector.Detect(root).candidates) {
    times[candidate.rel_path] = fs::last_write_time(root / candidate.rel_path, ec);
  }
  return times;
}

// `existed`: executables there before the installer ran, which aren't what it installed unless it
// rewrote them (a repack's checksum tool beside its setup.exe).
std::optional<model::Candidate> FirstGameExe(const Detector::Result& detected, const fs::path& root,
                                             const fs::path& installer,
                                             const ExeTimes& existed = {}) {
  std::error_code ec;
  for (const model::Candidate& candidate : detected.candidates) {
    if (candidate.is_installer || fs::equivalent(root / candidate.rel_path, installer, ec))
      continue;
    if (const auto it = existed.find(candidate.rel_path);
        it != existed.end() && fs::last_write_time(root / candidate.rel_path, ec) == it->second) {
      continue;
    }
    return candidate;
  }
  return std::nullopt;
}

// Where a quiet install puts the game: C:\Games\<name> in its prefix, where an installer left to
// itself would put it, so the installer's own folder keeps only the installer.
fs::path SilentTarget(const model::Game& game, const fs::path& prefix) {
  std::string name = game.name;
  std::erase_if(name, [](char c) { return std::string_view("<>:\"/\\|?*").contains(c); });
  if (name.empty()) name = game.id;
  return prefix / "drive_c" / "Games" / name;
}

constexpr std::string_view kInstallLogMark = "[mira] install ";

// The game's log, where its log window reads: a launch's is written by mira-run, an install's here.
// Installer runs add to one log, so a quiet run's output survives the shown run tried after it; a
// launch's log is kept as <log>.1, as mira-run keeps it.
std::ofstream OpenInstallLog(const fs::path& log_file) {
  std::error_code ec;
  fs::create_directories(log_file.parent_path(), ec);
  std::string first_line;
  std::getline(std::ifstream(log_file), first_line);
  if (first_line.starts_with(kInstallLogMark)) return std::ofstream(log_file, std::ios::app);
  fs::rename(log_file, log_file.string() + ".1", ec);
  return std::ofstream(log_file, std::ios::binary | std::ios::trunc);
}

Result<model::Game> RunInstaller(config::Config& config, const model::Game& game, InstallMode mode,
                                 const fs::path& log_file) {
  const fs::path installer = fs::path(game.install_path) / game.exe_path;
  const InstallerFormat format = DetectInstallerFormat(installer);
  const bool silent = format != InstallerFormat::kUnknown && mode != InstallMode::kInteractive;
  if (!silent && mode == InstallMode::kSilentOnly) {
    return Err("installer_unsupported", "not a known silent-install format",
               "Run the installer with its window shown and click through it.",
               Fix::Game(game.id, "install"));
  }

  const runner::RunnerRegistry runners(config);
  model::Game to_provision = game;
  if (to_provision.runner_ref.empty()) to_provision.runner_ref = config.GetString("install.runner");
  const model::Game provisioned = runners.ProvisionGame(to_provision);
  if (provisioned.status == model::GameStatus::Broken) {
    return Err("provision_failed",
               "couldn't set up a Wine prefix for the installer: " + provisioned.last_error,
               "The runner may be broken. Try a different one.", Fix::Runners());
  }
  const Result<runner::RunnerRegistry::Resolved> resolved = runners.Resolve(provisioned.runner_ref);
  if (!resolved) return std::unexpected(resolved.error());

  model::Game run_as = provisioned;
  run_as.args = silent ? SilentArgsFor(config, format) : "";
  const Result<Command> command = resolved->runner->BuildCommand(run_as, resolved->build);
  if (!command) return std::unexpected(command.error());

  Command run = *command;
  // In a scope of its own, as a game runs: an unpacker that runs the system out of memory is what
  // systemd-oomd kills, not mirad and the GUI with it. The scope execs the installer, so its output
  // and exit code are still the installer's.
  if (const auto systemd_run = runner::FindOnPath("systemd-run")) {
    run.argv.insert(run.argv.begin(), {*systemd_run, "--user", "--scope", "--quiet", "--collect",
                                       "--description", "Mira: installing " + game.name, "--"});
  }
  const fs::path target = SilentTarget(provisioned, provisioned.data_dir);
  if (silent) {
    std::string windows_target =
        "C:\\" + target.lexically_relative(fs::path(provisioned.data_dir) / "drive_c").string();
    std::ranges::replace(windows_target, '/', '\\');
    const char* flag = format == InstallerFormat::kInnoSetup ? "/DIR="
                       : format == InstallerFormat::kMsi     ? "TARGETDIR="
                                                             : "/D=";
    run.argv.push_back(flag + windows_target);
  }
  // Inno Setup prints nothing; its own log says what a failed run was doing. Copied into the
  // game's log after the run.
  const fs::path inno_log = fs::path(provisioned.data_dir) / "drive_c" / "mira-install.log";
  if (format == InstallerFormat::kInnoSetup) {
    std::error_code ec;
    fs::remove(inno_log, ec);
    run.argv.push_back("/LOG=C:\\mira-install.log");
  }

  const Detector detector(SettingsFromConfig(config));
  const ExeTimes existed = Executables(detector, game.install_path);
  const std::set<fs::path> before =
      InstallDirs(config.GetStringArray("install.detect_dirs"), provisioned.data_dir);
  const std::uintmax_t baseline_bytes = TreeBytes(game.install_path);
  {
    const std::lock_guard lock(tracked_mutex);
    Tracked& entry = tracked[game.id];
    entry.progress.state = "running";
    entry.progress.mode = silent ? "silent" : "interactive";
    entry.progress.started_at = model::NowSeconds();
    entry.install_path = game.install_path;
    entry.baseline_bytes = baseline_bytes;
    entry.data_dir = provisioned.data_dir;
    entry.detect_dirs = config.GetStringArray("install.detect_dirs");
    entry.drive_c_before = before;
  }
  log::Info("running {} installer for {}: {}", silent ? "silent" : "interactive", game.id,
            installer.string());
  std::ofstream install_log = OpenInstallLog(log_file);
  const auto write_log = [&](std::string_view text) {
    install_log << text;
    install_log.flush();
  };
  std::string command_line;
  for (const std::string& arg : run.argv) command_line += (command_line.empty() ? "" : " ") + arg;
  write_log(std::format("{}({}): {}\n", kInstallLogMark, silent ? "silent" : "interactive",
                        command_line));
  const Result<runner::ExecResult> result = runner::RunAndWait(run, write_log);
  write_log(result ? std::format("\n[mira] installer exited with code {}\n", result->exit_code)
                   : std::format("\n[mira] installer didn't run: {}\n", result.error().message));
  if (std::ifstream inno(inno_log, std::ios::binary); inno) {
    write_log("[mira] Inno Setup's log:\n");
    write_log(std::string(std::istreambuf_iterator<char>(inno), {}));
    inno.close();
    std::error_code ec;
    fs::remove(inno_log, ec);
  }
  if (!result) {
    // Wine's own processes leave the installer's group, so a cancel ends them through the prefix.
    if (result.error().code == "cancelled" && !provisioned.data_dir.empty()) {
      for (pid_t found : proc::FindPrefixProcesses(provisioned.data_dir)) ::kill(found, SIGKILL);
    }
    return std::unexpected(result.error());
  }
  // A GUI installer's exit code isn't reliable; detection below decides.
  if (silent && result->exit_code != 0) {
    return Err(
        "installer_failed",
        std::format("the quiet install failed (exit code {})", result->exit_code),
        "Some installers only work with their window shown. Run it that way and click through it.",
        Fix::Game(game.id, "install"));
  }

  model::Game done = provisioned;
  Detector::Result detected;
  std::optional<model::Candidate> exe;
  if (silent) {
    detected = detector.Detect(target);
    exe = FirstGameExe(detected, target, installer);
    if (exe) done.install_path = target.string();
  }
  if (!exe) {
    detected = detector.Detect(game.install_path);
    exe = FirstGameExe(detected, game.install_path, installer, existed);
  }
  for (const fs::path& dir :
       InstallDirs(config.GetStringArray("install.detect_dirs"), provisioned.data_dir)) {
    if (exe) break;
    if (before.contains(dir)) continue;
    detected = detector.Detect(dir);
    exe = FirstGameExe(detected, dir, installer);
    if (!exe) continue;
    const fs::path folder = GameFolder(dir, exe->rel_path);
    if (folder != dir) {
      detected = detector.Detect(folder);
      exe = FirstGameExe(detected, folder, installer);
    }
    if (exe) done.install_path = folder.string();
  }
  if (!exe) {
    return Err("no_executable", "the installer finished, but Mira couldn't find the installed game",
               "Choose the installed game's executable, then mark it installed.",
               Fix::Game(game.id, "exe"));
  }

  done.exe_path = exe->rel_path;
  done.candidates = detected.candidates;
  done.confidence = detected.confidence;
  done.status = model::GameStatus::Ready;
  done.last_error.clear();
  return done;
}

}  // namespace

std::string_view ToString(InstallerFormat format) {
  switch (format) {
    case InstallerFormat::kInnoSetup:
      return "inno";
    case InstallerFormat::kNsis:
      return "nsis";
    case InstallerFormat::kMsi:
      return "msi";
    case InstallerFormat::kUnknown:
      return "unknown";
  }
  return "unknown";
}

std::set<fs::path> InstallFolders(const config::Config& config, const fs::path& prefix) {
  return InstallDirs(config.GetStringArray("install.detect_dirs"), prefix);
}

std::optional<InstalledApp> NewInstall(const config::Config& config, const fs::path& prefix,
                                       const std::set<fs::path>& before) {
  // Wine's own, which a Proton or Wine update can add to an existing prefix.
  static const std::set<std::string> kWineFolders = {"Common Files", "Internet Explorer",
                                                     "Windows Media Player", "Windows NT"};
  const Detector detector(SettingsFromConfig(config));
  std::optional<InstalledApp> found;
  for (const fs::path& dir : InstallFolders(config, prefix)) {
    if (before.contains(dir) || kWineFolders.contains(dir.filename().string())) continue;
    Detector::Result detected = detector.Detect(dir);
    auto exe = std::ranges::find(detected.candidates, false, &model::Candidate::is_installer);
    if (exe != detected.candidates.end()) {
      const fs::path folder = GameFolder(dir, exe->rel_path);
      if (folder == dir) return InstalledApp{dir, exe->rel_path};
      detected = detector.Detect(folder);
      exe = std::ranges::find(detected.candidates, false, &model::Candidate::is_installer);
      if (exe != detected.candidates.end()) return InstalledApp{folder, exe->rel_path};
    }
    if (!found) found = InstalledApp{dir, ""};
  }
  return found;
}

Result<InstallerInfo> DescribeInstaller(const config::Config& config, const model::Game& game) {
  if (game.exe_path.empty()) return runner::NoExecutable(game);
  InstallerInfo info;
  info.path = fs::path(game.install_path) / game.exe_path;
  std::error_code ec;
  info.size_bytes = fs::file_size(info.path, ec);
  if (ec) {
    return Err("installer_missing", std::format("there's no installer at {}", info.path.string()),
               "Pick the installer again.");
  }
  info.format = DetectInstallerFormat(info.path);
  info.silent = info.format != InstallerFormat::kUnknown;
  info.silent_args = SilentArgsFor(config, info.format);
  const std::vector<fs::path> roots = config.GetPathArray("library_roots");
  if (!paths::IsWithin(game.install_path, roots, /*allow_equal=*/true)) {
    for (const fs::path& root : roots) {
      const fs::space_info space = fs::space(root, ec);
      if (!ec) info.targets.push_back({root, space.available});
    }
  }
  return info;
}

std::optional<InstallProgress> Progress(const std::string& id) {
  Tracked entry;
  {
    const std::lock_guard lock(tracked_mutex);
    const auto it = tracked.find(id);
    if (it == tracked.end()) return std::nullopt;
    entry = it->second;
  }
  if (entry.progress.state == "queued") return entry.progress;
  // Walking a big install tree is slow; polls in between get the last count.
  const auto checked = std::chrono::steady_clock::now();
  if (checked - entry.measured_at < std::chrono::seconds(2)) return entry.progress;
  const std::uintmax_t now = TreeBytes(entry.install_path);
  std::uintmax_t written = now > entry.baseline_bytes ? now - entry.baseline_bytes : 0;
  for (const fs::path& dir : InstallDirs(entry.detect_dirs, entry.data_dir)) {
    if (!entry.drive_c_before.contains(dir)) written += TreeBytes(dir);
  }
  entry.progress.bytes_written = written;
  const std::lock_guard lock(tracked_mutex);
  if (const auto it = tracked.find(id); it != tracked.end()) {
    it->second.progress.bytes_written = written;
    it->second.measured_at = checked;
  }
  return entry.progress;
}

bool Installing(const std::string& id) {
  const std::lock_guard lock(tracked_mutex);
  const auto it = tracked.find(id);
  return it != tracked.end() && it->second.progress.state == "running";
}

bool AutoInstalls(const config::Config& config, const model::Game& game) {
  if (game.status != model::GameStatus::NeedsInstall) return false;
  if (!config.GetBool("auto_setup") || !config.GetBool("scan.auto_run_installers")) return false;
  if (!config.GetBool("install.retry_failed") &&
      game.last_error.starts_with("Install didn't finish"))
    return false;
  return DetectInstallerFormat(fs::path(game.install_path) / game.exe_path) !=
         InstallerFormat::kUnknown;
}

void AdoptInstallFolder(model::Game& game, const std::string& install_path) {
  if (install_path.empty() || install_path == game.install_path) return;
  // Still the name cleaned from the installer's folder, not one the user gave: take the installed
  // folder's. Cleaning the stored name too matches an older CleanGameName's ("setup crate escape").
  if (strings::CleanGameName(game.name) ==
      strings::CleanGameName(fs::path(game.install_path).filename().string())) {
    game.name = strings::CleanGameName(fs::path(install_path).filename().string());
  }
  if (game.installer_dir.empty()) game.installer_dir = game.install_path;
  game.install_path = install_path;
}

void AnnounceInstallerLeftover(const config::Config& config, api::EventBus& events,
                               const model::Game& game) {
  std::error_code ec;
  if (game.installer_dir.empty() || !fs::is_directory(game.installer_dir, ec)) return;
  // A disc or someone else's folder isn't Mira's to offer to delete.
  if (!paths::IsWithin(game.installer_dir, config.GetPathArray("library_roots"))) return;
  events.Publish("game.installer_leftover", {{"id", game.id},
                                             {"installer_dir", game.installer_dir},
                                             {"bytes", TreeBytes(game.installer_dir)}});
}

Result<void> DeleteInstallerFolder(const config::Config& config, const model::Game& game) {
  if (game.installer_dir.empty()) {
    return Err("no_installer_dir", "this game has no installer folder left over from its install");
  }
  const fs::path folder = fs::weakly_canonical(game.installer_dir);
  for (const std::string& kept : {game.install_path, game.data_dir}) {
    if (!kept.empty() &&
        paths::IsWithin(fs::weakly_canonical(kept), {folder}, /*allow_equal=*/true)) {
      return Err("installer_dir_in_use", "the installer folder also holds the game or its prefix");
    }
  }
  if (auto deleted = DeleteInside(game.installer_dir, config.GetPathArray("library_roots"));
      !deleted) {
    return std::unexpected(deleted.error());
  }
  return {};
}

Result<void> RunInstall(config::Config& config, store::GameStore& games, api::EventBus& events,
                        metadata::FetchQueue* fetches, const std::string& id, InstallMode mode,
                        const std::optional<fs::path>& installer,
                        const std::optional<fs::path>& target) {
  events.Publish("game.install.started", {{"id", id}});
  const Result<model::Game> done = Install(config, games, id, mode, installer, target);
  if (!done) {
    if (const auto stored = games.Find(id)) events.Publish("game.updated", model::ToJson(*stored));
    events.Publish("game.install.failed", api::FailedEvent({{"id", id}}, done.error()));
    return std::unexpected(done.error());
  }
  if (auto synced = desktop::DesktopEntries(config, games.Metadata()).Sync(games.All()); !synced) {
    log::Warn("could not update application menu entries: {}", synced.error().message);
  }
  events.Publish("game.updated", model::ToJson(*done));
  events.Publish("game.install.finished", {{"id", id}});
  AnnounceInstallerLeftover(config, events, *done);
  // Its art and store info were looked up by the installer's name.
  if (fetches != nullptr) fetches->EnqueueNew(config, events, *done);
  return {};
}

bool BeginInstall(const std::string& id) {
  const std::lock_guard lock(tracked_mutex);
  const auto it = tracked.find(id);
  if (it != tracked.end() &&
      (it->second.progress.state == "queued" || it->second.progress.state == "running")) {
    return false;
  }
  Tracked entry;
  entry.progress.state = "queued";
  tracked[id] = entry;
  return true;
}

static Result<model::Game> InstallImpl(config::Config& config, store::GameStore& games,
                                       const std::string& id, InstallMode mode,
                                       const std::optional<fs::path>& installer,
                                       const std::optional<fs::path>& target) {
  const std::lock_guard run_lock(install_run_mutex);
  std::optional<model::Game> game = games.Find(id);
  if (!game) {
    Finish(id, "no such game");
    return Err("game_not_found", "no such game");
  }
  const bool installable = game->status == model::GameStatus::NeedsInstall ||
                           (installer && game->status == model::GameStatus::Broken);
  if (!installable) {
    Finish(id, "game isn't waiting on an installer");
    return Err("not_needs_install", "game isn't waiting on an installer");
  }
  if (installer) {
    std::error_code ec;
    if (!fs::is_regular_file(*installer, ec)) {
      Finish(id, "installer not found");
      return Err("installer_missing",
                 std::format("there's no installer at {}", installer->string()),
                 "Pick the installer again.");
    }
    // Relative when it sits in the game folder, so a relocate keeps it valid.
    const fs::path relative = installer->lexically_relative(game->install_path);
    const bool inside = !relative.empty() && *relative.begin() != "..";
    auto saved = games.Update(id, [&](model::Game& stored) {
      stored.exe_path = inside ? relative.string() : installer->string();
      stored.status = model::GameStatus::NeedsInstall;
    });
    if (!saved) {
      Finish(id, saved.error().message);
      return std::unexpected(saved.error());
    }
    game = *saved;
  }

  // The game installs into its prefix, so a disc or mounted image (full or read-only) mustn't hold
  // it: it goes on the target root's disk. One already in Mira's prefix folders (an earlier try)
  // is kept.
  std::error_code ec;
  if (target &&
      (!fs::exists(game->data_dir, ec) || !paths::IsWithin(game->data_dir, PrefixRoots(config)))) {
    model::Game placed = *game;
    placed.install_path = target->string();  // the root itself, so never a game folder's own
    game->data_dir = PrefixDir(config, games, placed).string();
  }
  const Result<model::Game> done =
      RunInstaller(config, *game, mode, proc::GameLogPath(games.Dir(), id));
  Finish(id, done ? "" : done.error().message);
  if (done) {
    log::Info("installed {}: {} in {}", id, done->exe_path, done->install_path);
  } else {
    log::Warn("install for {} failed: {}", id, done.error().message);
  }
  auto saved = games.Update(id, [&](model::Game& stored) {
    if (done) {
      AdoptInstallFolder(stored, done->install_path);
      stored.exe_path = done->exe_path;
      stored.candidates = done->candidates;
      stored.confidence = done->confidence;
      stored.runner_ref = done->runner_ref;
      stored.data_dir = done->data_dir;
      stored.status = done->status;
      stored.last_error.clear();
    } else {
      stored.data_dir = game->data_dir;  // a prefix moved to the target's disk was made there
      stored.last_error =
          std::format("Install didn't finish: {}. Run the installer again with its window shown.",
                      done.error().message);
    }
    stored.updated_at = model::NowSeconds();
  });
  if (!saved) return std::unexpected(saved.error());
  if (!done) return std::unexpected(done.error());
  return *saved;
}

Result<model::Game> Install(config::Config& config, store::GameStore& games, const std::string& id,
                            InstallMode mode, const std::optional<fs::path>& installer,
                            const std::optional<fs::path>& target) {
  // A throw would otherwise leave the install "running", refusing the game forever.
  try {
    return InstallImpl(config, games, id, mode, installer, target);
  } catch (const std::exception& error) {
    Finish(id, error.what());
    return Err("internal_error", error.what());
  }
}

}  // namespace mira::library
