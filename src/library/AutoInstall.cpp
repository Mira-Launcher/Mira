#include "library/AutoInstall.h"

#include <signal.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

#include "api/EventBus.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "desktop/DesktopEntries.h"
#include "gog/GameInfo.h"
#include "library/Detector.h"
#include "library/GameSetup.h"
#include "library/InstallSize.h"
#include "library/PeVersion.h"
#include "library/PrefixNaming.h"
#include "library/Relocate.h"
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

// The folders `pattern` names under `root`, where a "*" part is any folder ("users/*/AppData").
std::vector<fs::path> ExpandDirs(const fs::path& root, const fs::path& pattern) {
  std::vector<fs::path> dirs = {root};
  std::error_code ec;
  for (const fs::path& part : pattern) {
    std::vector<fs::path> next;
    for (const fs::path& dir : dirs) {
      if (part != "*") {
        next.push_back(dir / part);
        continue;
      }
      for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec)) next.push_back(entry.path());
      }
    }
    dirs = std::move(next);
  }
  return dirs;
}

// Top-level folders under install.detect_dirs in the prefix's drive_c.
std::set<fs::path> InstallDirs(const std::vector<std::string>& parents, const fs::path& prefix) {
  std::set<fs::path> dirs;
  std::error_code ec;
  for (const std::string& parent : parents) {
    for (const fs::path& dir : ExpandDirs(prefix / "drive_c", parent)) {
      for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec)) dirs.insert(entry.path());
      }
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

// An installer's own children (Inno's setup.tmp, msiexec, a bundled sub-installer) can outlive it.
// Waits for them, but not for Wine's services, up to `limit`.
void WaitForInstallerChildren(const fs::path& prefix, std::chrono::seconds limit) {
  static const std::set<std::string> kWineServices = {
      "services.exe", "winedevice.exe", "plugplay.exe", "explorer.exe", "rpcss.exe",
      "svchost.exe",  "conhost.exe",    "start.exe",    "wineboot.exe", "tabtip.exe"};
  const auto installer_running = [&] {
    for (pid_t pid : proc::FindPrefixProcesses(prefix.string())) {
      std::string argv0;
      std::getline(std::ifstream(std::format("/proc/{}/cmdline", pid)), argv0, '\0');
      std::ranges::replace(argv0, '\\', '/');
      const std::string name = strings::ToLower(fs::path(argv0).filename().string());
      if (kWineServices.contains(name)) continue;
      if (name.ends_with(".exe") || name.ends_with(".tmp") || name.ends_with(".msi")) return true;
    }
    return false;
  };
  const auto until = std::chrono::steady_clock::now() + limit;
  while (installer_running() && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

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
  if (provisioned.status == model::GameStatus::NotSetUp) {
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
  // An installer in a game's own folder may install beside itself; one loose in Downloads or on a
  // disc never does, and that folder's other programs aren't the game.
  const bool own_folder =
      paths::IsWithin(game.install_path, config.GetPathArray("library_roots"), false);
  const ExeTimes existed = own_folder ? Executables(detector, game.install_path) : ExeTimes{};
  const std::set<fs::path> before = ProgramFolders(config, provisioned.data_dir);
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
    // Wine's own processes leave the installer's group, so they're ended through the prefix.
    if (!provisioned.data_dir.empty()) {
      for (pid_t found : proc::FindPrefixProcesses(provisioned.data_dir)) ::kill(found, SIGKILL);
    }
    return std::unexpected(result.error());
  }
  WaitForInstallerChildren(provisioned.data_dir, std::chrono::minutes(15));
  // A GUI installer's exit code isn't reliable; detection below decides.
  // An MSI's 3010 and 1641: installed, wants a restart.
  const bool wants_restart =
      format == InstallerFormat::kMsi && (result->exit_code == 3010 || result->exit_code == 1641);
  if (silent && result->exit_code != 0 && !wants_restart) {
    // A later try is told apart by being new, so nothing of this one stays.
    if (std::error_code ec; !before.contains(target)) fs::remove_all(target, ec);
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
  if (!exe && own_folder) {
    detected = detector.Detect(game.install_path);
    exe = FirstGameExe(detected, game.install_path, installer, existed);
  }
  if (!exe) {
    if (const auto app = NewInstall(config, provisioned.data_dir, before);
        app && !app->exe_path.empty()) {
      detected = detector.Detect(app->dir);
      const auto found =
          std::ranges::find(detected.candidates, app->exe_path, &model::Candidate::rel_path);
      if (found != detected.candidates.end()) {
        exe = *found;
        done.install_path = app->dir.string();
      }
    }
  }
  if (!exe) {
    return Err("no_executable", "the installer finished, but Mira couldn't find the installed game",
               "Choose the installed game's program.", Fix::Game(game.id, "exe"));
  }

  done.exe_path = exe->rel_path;
  done.candidates = detected.candidates;
  // The user confirms what was found; a store's game was never a guess.
  const bool confirm = game.source == "scan" || game.source == "manual";
  done.setup = confirm ? model::SetupStep::Confirm : model::SetupStep::None;
  done.status = confirm ? model::GameStatus::NotSetUp : RunnableStatus(done);
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

namespace {

// Wine's own, which a Proton or Wine update can add to an existing prefix.
bool IsWineFolder(const fs::path& dir) {
  static const std::set<std::string> kWineFolders = {"Common Files", "Internet Explorer",
                                                     "Windows Media Player", "Windows NT"};
  return kWineFolders.contains(dir.filename().string());
}

// True when `name` matches one of `globs`, ignoring case.
bool MatchesAny(const std::vector<std::string>& globs, const std::string& name) {
  return std::ranges::any_of(globs, [&](const std::string& glob) {
    return strings::GlobMatch(strings::ToLower(glob), strings::ToLower(name));
  });
}

// The program in a new folder `dir`, inside the game's own folder under a publisher's. Installers
// and programs named by `skip` aren't it.
std::optional<InstalledApp> FindApp(const Detector& detector, const fs::path& dir,
                                    const std::vector<std::string>& skip = {}) {
  const auto pick = [&](const fs::path& folder) -> std::optional<std::string> {
    for (const model::Candidate& candidate : detector.Detect(folder).candidates) {
      if (!candidate.is_installer && !MatchesAny(skip, fs::path(candidate.rel_path).filename()))
        return candidate.rel_path;
    }
    return std::nullopt;
  };
  const auto exe = pick(dir);
  if (!exe) return std::nullopt;
  const fs::path folder = GameFolder(dir, *exe);
  if (folder == dir) return InstalledApp{dir, *exe};
  const auto inner = pick(folder);
  if (!inner) return std::nullopt;
  return InstalledApp{folder, *inner};
}

}  // namespace

std::optional<InstalledApp> NewInstall(const config::Config& config, const fs::path& prefix,
                                       const std::set<fs::path>& before) {
  std::optional<InstalledApp> best;
  std::uintmax_t best_bytes = 0;
  for (InstalledApp& app : NewPrograms(config, prefix, before)) {
    const std::uintmax_t bytes = TreeBytes(app.dir);
    if (best && bytes <= best_bytes) continue;
    best_bytes = bytes;
    best = std::move(app);
  }
  return best;
}

namespace {

// A value of a Wine .reg file, unquoted and unescaped.
std::string RegValue(std::string_view raw) {
  if (raw.size() < 2 || raw.front() != '"') return {};
  std::string out;
  for (std::size_t i = 1; i < raw.size() && raw[i] != '"'; ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) ++i;
    out += raw[i];
  }
  return out;
}

// `path` with each part as it's spelled on disk, since Windows paths ignore case.
fs::path OnDisk(const fs::path& base, const fs::path& rest) {
  fs::path at = base;
  std::error_code ec;
  for (const fs::path& part : rest) {
    if (part.empty() || part == ".") continue;
    fs::path next = at / part;
    if (!fs::exists(next, ec)) {
      const std::string want = strings::ToLower(part.string());
      for (const fs::directory_entry& entry : fs::directory_iterator(at, ec)) {
        if (strings::ToLower(entry.path().filename().string()) == want) next = entry.path();
      }
    }
    at = std::move(next);
  }
  return at;
}

// Where a Windows path ("C:\Hollow Knight\unins000.exe" /SILENT) is: its drive through the
// prefix's dosdevices, so Z: is / and a game installed outside drive C is found too.
fs::path FromWindows(const fs::path& prefix, std::string windows) {
  if (windows.starts_with('"')) {
    windows.erase(0, 1);
    if (const std::size_t quote = windows.find('"'); quote != std::string::npos)
      windows.resize(quote);
  } else if (const std::size_t exe = strings::ToLower(windows).find(".exe ");
             exe != std::string::npos) {
    windows.resize(exe + 4);  // an unquoted uninstaller's arguments
  }
  if (windows.size() < 3 || windows[1] != ':') return {};
  std::ranges::replace(windows, '\\', '/');
  const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(windows[0])));
  std::error_code ec;
  fs::path drive = fs::canonical(prefix / "dosdevices" / std::string{letter, ':'}, ec);
  if (ec) {
    if (letter != 'c') return {};
    drive = fs::weakly_canonical(prefix / "drive_c", ec);
  }
  return OnDisk(drive, fs::path(windows.substr(3)).lexically_normal());
}

// An uninstaller an installer registered, as Inno, NSIS and MSI installers do.
struct Uninstall {
  std::string name;
  fs::path folder;              // InstallLocation, else the uninstaller's folder
  std::vector<fs::path> paths;  // the folder, the uninstaller and its icon
};

std::vector<Uninstall> Uninstalls(const fs::path& prefix) {
  std::vector<Uninstall> found;
  for (const char* file : {"system.reg", "user.reg"}) {
    std::ifstream reg(prefix / file);
    std::optional<Uninstall> entry;
    fs::path location, uninstaller, icon;
    const auto close = [&] {
      if (!entry) return;
      entry->folder = !location.empty()      ? location
                      : !uninstaller.empty() ? uninstaller.parent_path()
                                             : icon.parent_path();
      while (!entry->folder.has_filename() && entry->folder.has_relative_path())
        entry->folder = entry->folder.parent_path();
      for (const fs::path& path : {location, uninstaller, icon})
        if (!path.empty()) entry->paths.push_back(path);
      if (!entry->folder.empty()) found.push_back(std::move(*entry));
      entry.reset();
      location.clear();
      uninstaller.clear();
      icon.clear();
    };
    for (std::string line; std::getline(reg, line);) {
      if (line.starts_with('[')) {
        close();
        if (line.find("\\\\CurrentVersion\\\\Uninstall\\\\") != std::string::npos)
          entry = Uninstall{};
        continue;
      }
      if (!entry) continue;
      const std::size_t equals = line.find("\"=");
      if (equals == std::string::npos) continue;
      const std::string key = line.substr(1, equals - 1);
      const std::string value = RegValue(std::string_view(line).substr(equals + 2));
      if (key == "DisplayName") entry->name = value;
      if (key == "InstallLocation") location = FromWindows(prefix, value);
      if (key == "UninstallString") uninstaller = FromWindows(prefix, value);
      if (key == "DisplayIcon") icon = FromWindows(prefix, value.substr(0, value.rfind(',')));
    }
    close();
  }
  return found;
}

// The DisplayName registered for an uninstaller of what's in `dir`.
std::string RegisteredName(const fs::path& prefix, const fs::path& dir) {
  std::error_code ec;
  const std::string inside = strings::ToLower(fs::weakly_canonical(dir, ec).string());
  for (const Uninstall& entry : Uninstalls(prefix)) {
    for (const fs::path& path : entry.paths) {
      const std::string at = strings::ToLower(fs::weakly_canonical(path, ec).string());
      if (!entry.name.empty() && (at == inside || at.starts_with(inside + "/"))) return entry.name;
    }
  }
  return {};
}

// A registered install folder narrow enough to be one program's: not a drive, the home folder,
// Windows itself or the Mira folder an installer ran from.
bool ProgramFolder(const fs::path& prefix, const fs::path& folder) {
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return false;
  const fs::path drive_c = fs::weakly_canonical(prefix / "drive_c", ec);
  const fs::path home = fs::weakly_canonical(paths::Home(), ec);
  if (folder == drive_c || folder == home || paths::IsWithin(home, {folder}, true)) return false;
  if (paths::IsWithin(folder, {drive_c / "windows"}, true)) return false;
  return std::distance(folder.begin(), folder.end()) > 2;
}

// GOG's goggame-<id>.info beside the game holds its name.
std::string GogName(const fs::path& dir) {
  const auto info = gog::ReadGameInfo(dir);
  return info ? info->name : std::string();
}

}  // namespace

std::string InstalledName(const fs::path& prefix, const InstalledApp& app) {
  std::vector<std::string> names = {GogName(app.dir)};
  if (!prefix.empty()) names.push_back(RegisteredName(prefix, app.dir));
  names.push_back(app.dir.filename().string());
  if (!app.exe_path.empty()) names.push_back(PeProductName(app.dir / app.exe_path));
  // "Hollow Knight version 1.5.78", as uninstall entries often say.
  static const std::regex kVersion(R"(\s+(version|ver\.?|v)\s*\d[\w.]*\s*$)", std::regex::icase);
  for (const std::string& name : names) {
    std::string clean = strings::CleanGameName(std::regex_replace(name, kVersion, ""));
    if (!strings::IsGenericName(clean)) return clean;
  }
  return {};
}

std::set<fs::path> ProgramFolders(const config::Config& config, const fs::path& prefix) {
  std::vector<std::string> parents = config.GetStringArray("install.detect_dirs");
  // Where per-user installers put a program, the few that use ProgramData, and a folder right in
  // C:\ ("C:\MyGame").
  parents.insert(parents.end(), {"users/*/AppData/Local", "users/*/AppData/Local/Programs",
                                 "users/*/AppData/Roaming", "ProgramData", ""});
  std::set<fs::path> dirs = InstallDirs(parents, prefix);
  // Wherever an installer said it installed, outside drive C too (another drive, or Z: for a
  // folder of the user's picked in its Browse…).
  for (const Uninstall& entry : Uninstalls(prefix))
    if (ProgramFolder(prefix, entry.folder)) dirs.insert(entry.folder);
  // Wine's own top folders, and the ones searched inside, aren't programs.
  static const std::set<std::string> kDriveRoot = {"windows", "users", "programdata",
                                                   "program files", "program files (x86)"};
  std::erase_if(dirs, [&](const fs::path& dir) {
    if (dir.parent_path() != prefix / "drive_c") return false;
    const std::string name = strings::ToLower(dir.filename().string());
    return kDriveRoot.contains(name) || std::ranges::any_of(parents, [&](const std::string& p) {
             return strings::ToLower(p) == name;
           });
  });
  return dirs;
}

std::vector<InstalledApp> NewPrograms(const config::Config& config, const fs::path& prefix,
                                      const std::set<fs::path>& before) {
  // What runtimes and redistributables install (.NET, the VC++ build tools, DirectX and XNA,
  // PhysX, OpenAL), which nobody starts themselves.
  static const std::vector<std::string> kRuntimeFolders = {"dotnet",       "microsoft*",
                                                           "windows kits", "reference assemblies",
                                                           "msbuild",      "nvidia corporation",
                                                           "*physx*",      "openal*",
                                                           "*redist*",     "temp"};
  // Besides detect.deny_name_patterns' helpers: a program's updater and uninstaller.
  static const std::vector<std::string> kNotOffered = {"update.exe", "*updater*", "*uninst*"};
  const DetectorSettings settings = SettingsFromConfig(config);
  std::vector<std::string> skip = settings.deny_name_patterns;
  skip.insert(skip.end(), kNotOffered.begin(), kNotOffered.end());
  const Detector detector(settings);
  std::vector<InstalledApp> apps;
  for (const fs::path& dir : ProgramFolders(config, prefix)) {
    if (before.contains(dir) || IsWineFolder(dir) ||
        MatchesAny(kRuntimeFolders, dir.filename().string()))
      continue;
    auto app = FindApp(detector, dir, skip);
    if (!app) continue;
    // The same program twice: a new folder holding another ("Local/Programs"), or one user's
    // folder linked to another's (Proton's users/<name> to steamuser).
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(app->dir, ec);
    if (!ec) app->dir = real;
    if (std::ranges::none_of(apps, [&](const InstalledApp& seen) {
          return seen.dir / seen.exe_path == app->dir / app->exe_path;
        })) {
      apps.push_back(std::move(*app));
    }
  }
  return apps;
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

namespace {
std::mutex requested_mutex;
std::set<fs::path> requested_installs;
}  // namespace

void RequestAutoInstall(const fs::path& folder) {
  const std::lock_guard lock(requested_mutex);
  requested_installs.insert(folder.lexically_normal());
}

bool IsAutoInstallRequested(const fs::path& folder) {
  const std::lock_guard lock(requested_mutex);
  return requested_installs.contains(folder.lexically_normal());
}

bool AutoInstalls(const config::Config& config, const model::Game& game) {
  if (!model::WaitsFor(game, model::SetupStep::Install)) return false;
  if (!config.GetBool("auto_setup") || !IsAutoInstallRequested(game.install_path)) return false;
  if (!config.GetBool("install.retry_failed") &&
      game.last_error.starts_with("Install didn't finish"))
    return false;
  return DetectInstallerFormat(fs::path(game.install_path) / game.exe_path) !=
         InstallerFormat::kUnknown;
}

void AdoptInstallFolder(model::Game& game, const std::string& install_path,
                        const std::string& exe_path, const std::string& prefix) {
  if (install_path.empty() || install_path == game.install_path) return;
  // Still the name cleaned from the installer's folder or file (one loose in Downloads), or one
  // that says nothing, not one the user gave: take the installed game's. Cleaning the stored name
  // too matches an older CleanGameName's ("setup crate escape").
  const std::string current = strings::CleanGameName(game.name);
  if (current == strings::CleanGameName(fs::path(game.install_path).filename().string()) ||
      current == strings::CleanGameName(fs::path(game.exe_path).stem().string()) ||
      strings::IsGenericName(current)) {
    if (std::string name = InstalledName(prefix, {install_path, exe_path}); !name.empty())
      game.name = std::move(name);
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

Result<void> DeleteInstallerFolder(const config::Config& config, const model::Game& game,
                                   const std::vector<model::Game>& library) {
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
  for (const model::Game& other : library) {
    if (other.id == game.id) continue;
    for (const std::string& kept : {other.install_path, other.data_dir}) {
      std::error_code ec;
      if (!kept.empty() &&
          paths::IsWithin(fs::weakly_canonical(kept, ec), {folder}, /*allow_equal=*/true))
        return Err("installer_dir_in_use",
                   std::format("the installer folder also holds {}", other.name));
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

// After an install renamed the game from its installer's name to the installed one, its prefix,
// still named after the old one, takes the new name.
static model::Game FollowName(const config::Config& config, store::GameStore& games,
                              model::Game game, const std::string& old_name) {
  if (game.name == old_name || game.data_dir.empty() || config.GetString("prefix_naming") != "name")
    return game;
  const fs::path prefix = game.data_dir;
  const std::string old_slug = strings::Slugify(old_name);
  const std::string leaf = prefix.filename().string();
  if (!paths::IsWithin(prefix, PrefixRoots(config)) ||
      (leaf != old_slug && !leaf.starts_with(old_slug + "-")))
    return game;
  const std::vector<model::Game> library = games.All();
  std::set<fs::path> taken;
  for (const model::Game& other : library) {
    if (other.id != game.id && !other.data_dir.empty()) taken.insert(other.data_dir);
  }
  const fs::path to = NamedDir(config, game, prefix.parent_path(), prefix, taken);
  if (to == prefix) return game;
  const auto moved = Relocate(
      config, game, {.install_path = std::nullopt, .data_dir = to, .only_given = true}, library);
  if (!moved) {
    log::Warn("couldn't rename {}'s prefix to {}: {}", game.id, to.string(), moved.error().message);
    return game;
  }
  auto saved = games.Update(game.id, [&](model::Game& stored) {
    stored.data_dir = moved->data_dir;
    stored.install_path = moved->install_path;
  });
  if (saved) return *saved;
  // Not recorded: put the folder back where the game still points.
  std::error_code ec;
  fs::rename(moved->data_dir, prefix, ec);
  log::Warn("couldn't save {}'s renamed prefix: {}", game.id, saved.error().message);
  return game;
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
  const bool installable = model::WaitsFor(*game, model::SetupStep::Install) ||
                           (installer && game->status == model::GameStatus::NotSetUp);
  if (!installable) {
    Finish(id, "game isn't waiting on an installer");
    return Err("no_installer", "game isn't waiting on an installer");
  }
  {
    const std::int64_t estimate = EstimatedInstallBytes(games.Metadata().Read(id));
    const std::lock_guard lock(tracked_mutex);
    tracked[id].progress.estimated_bytes = estimate;
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
      stored.status = model::GameStatus::NotSetUp;
      stored.setup = model::SetupStep::Install;
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
      AdoptInstallFolder(stored, done->install_path, done->exe_path, done->data_dir);
      stored.exe_path = done->exe_path;
      stored.candidates = done->candidates;
      runner::TakeRunner(stored, *done);
      stored.data_dir = done->data_dir;
      stored.status = done->status;
      stored.setup = done->setup;
      stored.reviewed = false;
      stored.last_error.clear();
    } else {
      stored.data_dir = game->data_dir;  // a prefix moved to the target's disk was made there
      // Stopped on purpose: nothing went wrong with it.
      if (done.error().code == "cancelled" || done.error().code == "shutting_down") return;
      // The installer ran: running it again won't help, picking what it installed will.
      stored.last_error =
          done.error().code == "no_executable"
              ? "The installer finished, but Mira couldn't find the game. Choose its program "
                "(it's in drive C of its prefix)."
              : std::format(
                    "Install didn't finish: {}. Run the installer again with its window shown.",
                    done.error().message);
    }
    stored.updated_at = model::NowSeconds();
  });
  if (!saved) return std::unexpected(saved.error());
  if (!done) return std::unexpected(done.error());
  return FollowName(config, games, *saved, game->name);
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
