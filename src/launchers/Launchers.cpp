#include "launchers/Launchers.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "core/Lane.h"
#include "core/Log.h"
#include "core/LogHub.h"
#include "core/Paths.h"
#include "core/StoreErrors.h"
#include "core/Strings.h"
#include "launchers/AdobeWine.h"
#include "launchers/Autodesk.h"
#include "launchers/Office.h"
#include "launchers/Shims.h"
#include "library/PrefixNaming.h"
#include "proc/ProcessIndex.h"
#include "proc/ProcessSupervisor.h"
#include "runner/Curl.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"
#include "runner/RunnerUpdates.h"
#include "runner/Winetricks.h"
#include "system/Packages.h"

namespace mira::launchers {
namespace {
namespace fs = std::filesystem;

std::mutex state_mutex;
std::map<std::string, std::string> states;    // launcher id -> running, finished, failed
std::vector<std::string> queued_office_apps;  // asked for while Microsoft 365 was installing

std::vector<std::string> TakeQueuedOfficeApps() {
  const std::lock_guard lock(state_mutex);
  return std::exchange(queued_office_apps, {});
}

void SetState(const Launcher& launcher, std::string state) {
  const std::lock_guard lock(state_mutex);
  states[launcher.id] = std::move(state);
}

// What the install is doing, for the live log: "setup:<launcher id>".
std::string LogChannel(const Launcher& launcher) {
  return "setup:" + launcher.id;
}

void Say(const Launcher& launcher, std::string_view line) {
  loghub::Append(LogChannel(launcher), std::string(line) + "\n");
  log::Info("{}: {}", launcher.name, line);
}

// Moves what a child wrote to `file` since `offset` into the live log.
void CopyNewOutput(const Launcher& launcher, const fs::path& file, std::uintmax_t& offset) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return;
  in.seekg(static_cast<std::streamoff>(offset));
  std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  // Whole lines only, so a line cut in two by the read is filtered as one. An "Unhandled
  // Exception:" heading waits for its next line too: whether it is noise depends on it.
  chunk.resize(chunk.find_last_of('\n') == std::string::npos ? 0 : chunk.find_last_of('\n') + 1);
  constexpr std::string_view kHeading = "Unhandled Exception:\n";
  if (chunk.ends_with(kHeading) &&
      (chunk.size() == kHeading.size() || chunk[chunk.size() - kHeading.size() - 1] == '\n')) {
    chunk.resize(chunk.size() - kHeading.size());
  }
  if (chunk.empty()) return;
  offset += chunk.size();
  loghub::Append(LogChannel(launcher), WithoutNoise(chunk));
}

std::vector<pid_t> orphans;  // installer processes left running, reaped by a later call

// Reaps what has exited, then keeps `pid` (if any) to reap later.
void ReapOrphans(pid_t pid = -1) {
  const std::lock_guard lock(state_mutex);
  if (pid > 0) orphans.push_back(pid);
  std::erase_if(orphans, [](pid_t orphan) { return ::waitpid(orphan, nullptr, WNOHANG) != 0; });
}

// The Windows user folder the launcher writes its settings under.
fs::path UserDir(const fs::path& prefix) {
  const fs::path users = prefix / "drive_c" / "users";
  std::error_code ec;
  if (fs::is_directory(users / "steamuser", ec)) return users / "steamuser";
  const char* user = std::getenv("USER");
  return users / (user ? user : "user");
}

void WriteIfMissing(const fs::path& file, std::string_view content) {
  std::error_code ec;
  if (fs::exists(file, ec)) return;
  fs::create_directories(file.parent_path(), ec);
  std::ofstream(file) << content;
}

// Lutris's defaults: both launchers misbehave under Wine with these on.
void WriteDefaults(const config::Config& config, const Launcher& launcher, const fs::path& prefix) {
  if (launcher.id == "ubisoft" && config.GetBool("launchers.ubisoft.disable_overlay")) {
    WriteIfMissing(UserDir(prefix) / "AppData/Local/Ubisoft Game Launcher/settings.yaml",
                   "overlay:\n  enabled: false\n  forceunhookgame: false\n  fps_enabled: false\n"
                   "  warning_enabled: false\nuser:\n  closebehavior: CloseBehavior_Close\n");
  }
  if (launcher.id == "battlenet" && config.GetBool("launchers.battlenet.disable_hw_accel")) {
    WriteIfMissing(UserDir(prefix) / "AppData/Roaming/Battle.net/Battle.net.config",
                   R"({"Client":{"HardwareAcceleration":"false","Sound":{"Enabled":"false"},)"
                   R"("Streaming":{"StreamingEnabled":"false"}}})");
  }
}

// `rel` (relative to drive_c), or the same file name anywhere under its top
// folder (the EA app sometimes nests its exe under a version folder). Office's
// "Updates" folder holds files it has downloaded but not installed: not a match.
std::optional<fs::path> FindFile(const fs::path& prefix, const fs::path& rel) {
  const fs::path expected = prefix / "drive_c" / rel;
  std::error_code ec;
  if (fs::is_regular_file(expected, ec)) return expected;
  const fs::path top = prefix / "drive_c" / *rel.begin() / *std::next(rel.begin());
  for (fs::recursive_directory_iterator it(top, fs::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec) && it->path().filename() == "Updates") {
      it.disable_recursion_pending();
      continue;
    }
    // Windows names are case-insensitive (QuickBooks ships QBW.EXE).
    if (strings::ToLower(it->path().filename().string()) ==
            strings::ToLower(rel.filename().string()) &&
        it->is_regular_file(ec)) {
      return it->path();
    }
  }
  return std::nullopt;
}

std::optional<fs::path> FindExe(const Launcher& launcher, const fs::path& prefix) {
  return FindFile(prefix, launcher.exe);
}

// Whether `file` is still running in the game's prefix: started from
// `setup_dir`, or relaunched from anywhere (the Office Deployment Tool
// restarts itself from another drive letter).
bool SetupRunning(const model::Game& game, const std::string& setup_dir, const std::string& file) {
  if (!proc::FindDirProcesses(game.data_dir, setup_dir).empty()) return true;
  proc::ProcessIndex index;
  index.Refresh();
  const std::string name = "/" + strings::ToLower(file);
  return std::ranges::any_of(index.Processes(), [&](const auto& item) {
    return proc::InPrefix(item.second.prefix, game.data_dir) && item.second.argv0.ends_with(name);
  });
}

// "4:05", or "1:02:03" past an hour.
std::string Clock(std::chrono::seconds total) {
  const auto seconds = total.count();
  return seconds >= 3600
             ? std::format("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60)
             : std::format("{}:{:02}", seconds / 60, seconds % 60);
}

struct Download {
  std::uint64_t done = 0;
  std::uint64_t total = 0;
  double speed = 0;  // bytes a second
};

std::string Megabytes(std::uint64_t bytes) {
  return bytes >= (1ull << 30) ? std::format("{:.2f} GB", bytes / double(1ull << 30))
                               : std::format("{:.0f} MB", bytes / double(1ull << 20));
}

// "46% downloading Microsoft 365 - 753 MB of 1.63 GB at 19.4 MB/s - 0:46 left": each phase of an
// install has its own percentage, which is the one the rest of the line is about. `elapsed` is the
// time in this phase.
std::string DownloadLine(const Launcher& launcher, const Download& download,
                         std::chrono::seconds elapsed) {
  std::string line =
      std::format("{:.0f}% downloading {} - {} of {}", 100.0 * download.done / download.total,
                  launcher.name, Megabytes(download.done), Megabytes(download.total));
  if (download.speed >= 100 * 1024) {
    line += std::format(" at {:.1f} MB/s - ", download.speed / (1024 * 1024));
    line += Clock(std::chrono::seconds(
                static_cast<long long>((download.total - download.done) / download.speed))) +
            " left";
  } else {
    line += " - " + Clock(elapsed) + " elapsed";
  }
  return line + "\n";
}

// `left` is how long the install should still take, when there is a guess to show.
std::string InstallLine(const Launcher& launcher, double percent, std::chrono::seconds elapsed,
                        double disk_speed, std::optional<std::chrono::seconds> left) {
  std::string line = std::format("{:.0f}% installing {}", percent, launcher.name);
  if (disk_speed >= 100 * 1024) line += std::format(" - {:.1f} MB/s", disk_speed / (1024 * 1024));
  line += " - " + Clock(elapsed) + " elapsed";
  if (left) line += " - about " + Clock(*left) + " left";
  return line + "\n";
}

// Asks everything in `prefix` to quit, then insists.
void StopPrefix(const std::string& prefix) {
  const auto alive = [&] { return !proc::FindPrefixProcesses(prefix).empty(); };
  for (pid_t pid : proc::FindPrefixProcesses(prefix)) ::kill(pid, SIGTERM);
  for (int i = 0; i < 20 && alive(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  for (pid_t pid : proc::FindPrefixProcesses(prefix)) ::kill(pid, SIGKILL);
}

// Stops everything running in `prefix`. The installer's own session has done its job.
void EndSession(const Launcher& launcher, const std::string& prefix) {
  if (proc::FindPrefixProcesses(prefix).empty()) return;
  Say(launcher, "Closing what the installer left running");
  StopPrefix(prefix);
}

// `is_done` replaces the check for `step.done`'s file.
Result<void> RunInstaller(config::Config& config, const Launcher& launcher, const Setup& step,
                          const model::Game& game, const std::function<void(double)>& on_progress,
                          const std::function<bool()>& is_done = {}) {
  ReapOrphans();
  const fs::path downloads = paths::UserDir() / "downloads";
  // No URL: `step.file` is an installer already on disk (the user's own), run where it is and kept.
  const bool local = step.url.empty();
  const fs::path setup = local ? fs::path(step.file) : downloads / step.file;
  const std::string file = setup.filename().string();
  const fs::path done = step.done.empty() ? fs::path(launcher.exe) : fs::path(step.done);
  const auto finished = [&] {
    return is_done ? is_done() : FindFile(game.data_dir, done).has_value();
  };
  std::error_code ec;
  fs::create_directories(downloads, ec);

  if (!local) {
    Say(launcher, std::format("Downloading {}", file));
    if (auto fetched = runner::CurlDownload(step.url, setup); !fetched) {
      return Err("download_failed",
                 std::format("couldn't download the {} installer: {}", launcher.name,
                             fetched.error().message),
                 kConnectionHint);
    }
  }

  const runner::RunnerRegistry runners(config);
  const auto resolved = runners.Resolve(game.runner_ref);
  if (!resolved) return std::unexpected(resolved.error());
  model::Game run_as = game;
  run_as.install_path = setup.parent_path().string();
  run_as.exe_path = file;
  run_as.args.clear();
  auto command = resolved->runner->BuildCommand(run_as, resolved->build);
  if (!command) return std::unexpected(command.error());
  // Proton silences Wine; its errors are often the only clue why a setup quit.
  command->env.try_emplace("WINEDEBUG", "fixme-all,err+all");
  std::string windows_downloads = "Z:" + downloads.string();
  std::ranges::replace(windows_downloads, '/', '\\');
  for (std::string arg : step.args) {
    if (const std::size_t at = arg.find("{downloads}"); at != std::string::npos) {
      arg.replace(at, std::string_view("{downloads}").size(), windows_downloads);
    }
    command->argv.push_back(std::move(arg));
  }

  Say(launcher, std::format("Running {}", file));
  // Its output goes to a file that the wait loop below copies into the live log.
  const fs::path output_file = downloads / (file + ".log");
  std::error_code remove_ec;
  fs::remove(output_file, remove_ec);
  const int output_fd = ::open(output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  const auto pid = runner::SpawnDetached(*command, output_fd);
  if (output_fd >= 0) ::close(output_fd);
  if (!pid) return std::unexpected(pid.error());
  std::uintmax_t copied = 0;
  const auto started = std::chrono::steady_clock::now();
  const auto started_at = fs::file_time_type::clock::now();
  auto last_note = started;
  int percent = -1;  // the last figure `step.progress` gave
  std::uint64_t last_bytes = step.bytes ? step.bytes(game.data_dir) : 0;
  auto last_sample = started;
  double speed = 0;           // bytes a second on disk, smoothed
  double download_speed = 0;  // bytes a second over the network, smoothed
  std::uint64_t last_downloaded = 0;
  double shown = 0;  // this phase's percentage; never goes backwards within it
  bool downloading = false;
  auto phase_started = started;
  auto deadline =
      started;  // when the install should end, by the latest guess; it only moves earlier
  // umu-run only exits once everything in the prefix has, and a freshly
  // installed launcher (and the EA app's background service) keeps
  // running. So wait for the setup process itself instead. Exit codes
  // aren't reliable either (EA's returns 768 on success); whether the
  // launcher exe exists afterwards decides.
  const std::string setup_dir = strings::ToLower("z:" + setup.parent_path().string());
  // Battle.net's setup hands off to a second stage, so the exe must exist too.
  int status = 0;
  while (::waitpid(*pid, &status, WNOHANG) != *pid) {
    CopyNewOutput(launcher, output_file, copied);
    if (step.progress) {
      // One line in the log that keeps updating (LogHub treats a leading "NN%" as a progress
      // reading).
      const auto office_percent = step.progress(game.data_dir, started_at);
      const auto fetched = step.download ? step.download(game.data_dir) : std::nullopt;
      if (office_percent || fetched) {
        const auto sampled = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(sampled - last_sample).count();
        if (step.bytes) {
          const std::uint64_t bytes = step.bytes(game.data_dir);
          if (seconds > 0 && bytes >= last_bytes)
            speed = speed == 0 ? (bytes - last_bytes) / seconds
                               : 0.6 * speed + 0.4 * ((bytes - last_bytes) / seconds);
          last_bytes = bytes;
        }
        std::optional<Download> download;
        if (fetched) {
          if (seconds > 0 && fetched->first >= last_downloaded) {
            const double rate = (fetched->first - last_downloaded) / seconds;
            download_speed = download_speed == 0 ? rate : 0.6 * download_speed + 0.4 * rate;
          }
          last_downloaded = fetched->first;
          download = Download{fetched->first, fetched->second, download_speed};
        }
        last_sample = sampled;
        // Two phases, each with its own percentage: the download (the installer reports nothing
        // while it runs, so the shim's byte counts stand in), then the installer's own figure,
        // which starts near 10% once the download is done.
        const bool fetching = download && download->done < download->total &&
                              !(office_percent && *office_percent >= 10);
        if (fetching != downloading || phase_started == started) {
          if (fetching != downloading) shown = 0;
          downloading = fetching;
          phase_started = sampled;
        }
        const auto in_phase =
            std::chrono::duration_cast<std::chrono::seconds>(sampled - phase_started);
        std::string line;
        if (fetching) {
          shown = std::max(shown, 100.0 * download->done / download->total);
          line = DownloadLine(launcher, *download, in_phase);
        } else {
          shown = std::max(
              shown,
              office_percent ? std::clamp((*office_percent - 10) * 100.0 / 90.0, 0.0, 100.0) : 0.0);
          // The installer's percentage moves in steps, so a guess made from it swings. Each new
          // guess can only pull the finish earlier, until it has passed: then the guess starts
          // over.
          std::optional<std::chrono::seconds> left;
          if (shown >= 3 && shown < 100) {
            const auto guess = sampled + std::chrono::seconds(static_cast<long long>(
                                             in_phase.count() * (100.0 - shown) / shown));
            deadline = deadline > sampled ? std::min(deadline, guess) : guess;
            left = std::chrono::duration_cast<std::chrono::seconds>(deadline - sampled);
          }
          line = InstallLine(launcher, shown, in_phase, speed, left);
        }
        if (static_cast<int>(shown) != percent && static_cast<int>(shown) % 5 == 0) {
          log::Info("{}: {} {}%", launcher.name, fetching ? "downloading" : "installing",
                    static_cast<int>(shown));
        }
        percent = static_cast<int>(shown);
        loghub::Append(LogChannel(launcher), line);
        if (on_progress) on_progress(shown / 100.0);
      }
    }
    if (const auto why = step.failed ? step.failed() : std::nullopt) {
      CopyNewOutput(launcher, output_file, copied);
      EndSession(launcher, game.data_dir);
      ReapOrphans(*pid);
      return Err("install_failed", *why);
    }
    if ((step.stops_when_done || !SetupRunning(game, setup_dir, file)) && finished()) {
      CopyNewOutput(launcher, output_file, copied);
      Say(launcher, std::format("{} finished", file));
      if (step.end_session || step.stops_when_done) EndSession(launcher, game.data_dir);
      ReapOrphans(*pid);
      // Downloaded again on every run, so keeping it only takes space (WebView2's is 200 MB).
      std::error_code remove_setup_ec;
      if (!local) fs::remove(setup, remove_setup_ec);
      return {};
    }
    if (ThisTaskStop().stop_requested()) {
      const bool cancelled = ThisTaskCancelled();
      if (cancelled) {
        ::kill(-*pid, SIGKILL);
        for (pid_t found : proc::FindPrefixProcesses(game.data_dir)) ::kill(found, SIGKILL);
      }
      ReapOrphans(*pid);
      return cancelled ? Err("cancelled", "the installer was cancelled")
                       : Err("shutting_down", "mirad stopped before the installer finished");
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    // Wine installers say little: a line now and then shows it hasn't stalled.
    const auto now = std::chrono::steady_clock::now();
    if (percent < 0 && now - last_note >= std::chrono::seconds(15)) {
      last_note = now;
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
      Say(launcher, std::format("Still running {} ({}:{:02})", file, elapsed / 60, elapsed % 60));
    }
  }
  CopyNewOutput(launcher, output_file, copied);
  if (!finished()) {
    const std::string how =
        WIFEXITED(status)     ? std::format("exited with code {}", WEXITSTATUS(status))
        : WIFSIGNALED(status) ? std::format("was killed by signal {}", WTERMSIG(status))
                              : "finished";
    return Err("launcher_not_installed",
               std::format("{} {} but {} wasn't installed", file, how, launcher.name),
               std::format("Its output, with Wine's errors, is in {}", output_file.string()));
  }
  return {};
}

// mshtml's registration (its text/html handler), skipped when the prefix was made with mshtml
// turned off.
Result<void> RegisterMshtml(const runner::RunnerRegistry& runners, const model::Game& game) {
  const auto resolved = runners.Resolve(game.runner_ref);
  if (!resolved) return std::unexpected(resolved.error());
  for (const char* dir : {"system32", "syswow64"}) {
    model::Game run_as = game;
    run_as.install_path = (fs::path(game.data_dir) / "drive_c/windows" / dir).string();
    run_as.exe_path = "regsvr32.exe";
    run_as.args = "/s mshtml.dll";
    auto command = resolved->runner->BuildCommand(run_as, resolved->build);
    if (!command) return std::unexpected(command.error());
    const auto ran = runner::RunAndWait(*command);
    if (!ran) return std::unexpected(ran.error());
    if (ran->exit_code != 0) {
      return Err("install_failed",
                 std::format("registering mshtml in {} failed (exit {})", dir, ran->exit_code));
    }
  }
  return {};
}

// Builds the install image with the user's Create Installer, then installs the app from it.
Result<void> InstallAutodesk(config::Config& config, const Launcher& launcher,
                             const model::Game& game, const fs::path& installer,
                             const std::function<void(double)>& on_progress) {
  const auto started = fs::file_time_type::clock::now();
  const auto image = [&] { return autodesk::FindImage(game.data_dir, started); };
  autodesk::ForgetImageBuilt(game.data_dir);
  Say(launcher, "Building the install image (Autodesk's installer downloads the app)");
  Setup create;
  create.file = installer.string();
  create.stops_when_done = true;
  const auto built = [&] { return autodesk::ImageBuilt(game.data_dir) && image().has_value(); };
  if (auto ran = RunInstaller(config, launcher, create, game, {}, built); !ran) return ran;
  const auto setup = image();
  if (auto prepared = autodesk::PrepareImage(*setup); !prepared) return prepared;
  // wintrust: its packages' certificates have expired and each loads a registry hive, which Wine
  // can't check or load. bcryptprimitives: its crash reporter is a Go service, which needs that
  // shim to start under Wine.
  constexpr std::array kShims = {shims::Shim{"wintrust", "wintrustw"},
                                 shims::Shim{"bcryptprimitives", "bcryptprimitivesw"}};
  const runner::RunnerRegistry runners(config);
  if (auto installed =
          shims::Install(config, runners, game, paths::UserDir() / "downloads", kShims);
      !installed) {
    return installed;
  }
  if (auto overridden = shims::Override(runners, game, kShims); !overridden) return overridden;
  // Some packages run PS2EXE-wrapped PowerShell steps, which need a PowerShell host Wine lacks.
  if (auto installed = shims::InstallPowerShellHost(config, game, paths::UserDir() / "downloads");
      !installed) {
    return installed;
  }
  Setup install;
  install.file = setup->string();
  install.args = {"-q"};
  install.failed = [watch = std::make_shared<autodesk::FailureWatch>(game.data_dir)] {
    return watch->Failed();
  };
  if (auto installed = RunInstaller(config, launcher, install, game, on_progress); !installed)
    return installed;
  // The image is several GB and only needed to install.
  std::error_code ec;
  fs::remove_all(setup->parent_path(), ec);
  return {};
}

// launchers.app_runner, downloaded when it isn't installed. Empty when that setting is.
Result<std::string> AppRunner(config::Config& config, const Launcher& launcher) {
  const std::string ref = config.GetString("launchers.app_runner");
  if (ref.empty() || ref == "auto" || runner::RunnerRegistry(config).Resolve(ref)) return ref;
  const std::size_t colon = ref.find(':');
  const std::string kind = ref.substr(0, colon);
  const std::string tag = ref.substr(colon + 1);
  const auto family = runner::FamilyFor(config, kind, "");
  if (!family) return std::unexpected(family.error());
  const auto releases = runner::ListFamilyReleases(*family);
  if (!releases) return std::unexpected(releases.error());
  const auto match = std::ranges::find(*releases, tag, &runner::ReleaseAsset::tag);
  if (match == releases->end()) {
    return Err("release_not_found", std::format("no {} release tagged \"{}\"", family->label, tag),
               "Set launchers.app_runner to an installed runner.");
  }
  Say(launcher, "Downloading " + tag);
  if (auto installed = runner::DownloadAndInstall(config, kind, *match); !installed) {
    return std::unexpected(installed.error());
  }
  return ref;
}

Result<model::Game> InstallInto(config::Config& config, store::GameStore& games,
                                const Launcher& launcher,
                                const std::function<void(double)>& on_progress,
                                const std::string& installer) {
  if (!launcher.installer.empty()) {
    std::error_code ec;
    if (installer.empty() || !fs::is_regular_file(installer, ec)) {
      return Err("installer_needed",
                 std::format("{} installs from your own installer", launcher.name),
                 launcher.installer);
    }
    if (!launcher.autodesk_code.empty() &&
        !autodesk::IsCreateInstaller(installer, launcher.autodesk_code)) {
      return Err("wrong_installer",
                 std::format("{} isn't the installer for {}",
                             fs::path(installer).filename().string(), launcher.name),
                 launcher.installer);
    }
  }
  // Before the prefix and downloads, rather than failing at the first winetricks verb.
  if (!launcher.tricks.empty()) {
    if (auto ready = system::RequireFeature("winetricks", launcher.name); !ready)
      return std::unexpected(ready.error());
  }
  if (launcher.id == "adobe") {
    if (auto ready = system::RequireFeature("adobe", launcher.name); !ready)
      return std::unexpected(ready.error());
  }
  const std::string id = GameId(launcher);
  model::Game game = games.Find(id).value_or(model::Game{});
  const bool existed = !game.id.empty();
  game.id = id;
  game.name = launcher.name;
  game.source = "launcher";
  game.source_ref = launcher.id;
  game.platform = model::Platform::Windows;
  // Creative Cloud is an app of its own, in the library beside Photoshop.
  if ((launcher.id == "adobe" || launcher.app) && !std::ranges::contains(game.tags, "app"))
    game.tags.push_back("app");
  for (const auto& [key, value] : launcher.env) game.env.try_emplace(key, value);
  if (!launcher.umu_store.empty()) game.runner_config["store"] = launcher.umu_store;
  if (game.runner_ref.empty() && launcher.app) {
    auto ref = AppRunner(config, launcher);
    if (!ref) return std::unexpected(ref.error());
    game.runner_ref = *ref;
  }
  if (game.runner_ref.empty()) game.runner_ref = config.GetString("launchers.runner");
  // Creative Cloud needs AdobeWine, the build launchers.adobe.adobewine_url picks, also in an entry
  // an older install left on another runner.
  if (launcher.id == "adobe" && config.GetString("launchers.runner").empty()) {
    auto ref =
        adobewine::Runtime(config, [&launcher](const std::string& line) { Say(launcher, line); });
    if (!ref) return std::unexpected(ref.error());
    // Another Wine's server still running in the prefix would refuse this one.
    if (*ref != game.runner_ref && !game.data_dir.empty()) EndSession(launcher, game.data_dir);
    game.runner_ref = *ref;
  }
  if (game.data_dir.empty()) game.data_dir = library::PrefixDir(config, games, game).string();
  game.status = model::GameStatus::SettingUp;
  game.last_error.clear();
  game.updated_at = model::NowSeconds();
  if (!existed) game.created_at = game.updated_at;
  if (auto saved = games.Upsert(game); !saved) return std::unexpected(saved.error());

  const runner::RunnerRegistry runners(config);
  std::error_code ec;
  // Provisioning an existing prefix waits on anything running in it, such
  // as the launcher itself.
  if (game.runner_ref.empty() || !fs::exists(fs::path(game.data_dir) / "drive_c", ec)) {
    Say(launcher, "Creating the Wine prefix (the first run downloads and sets up the runtime)");
    // An install mirad stopped part way leaves its Wine processes behind, and provisioning would
    // wait on them.
    if (!game.data_dir.empty()) EndSession(launcher, game.data_dir);
    const model::Game provisioned = runners.ProvisionGame(game);
    if (provisioned.status == model::GameStatus::Broken) {
      return Err("provision_failed",
                 "couldn't set up the launcher's Wine prefix: " + provisioned.last_error,
                 "The runner may be broken. Try a different one.", Fix::Runners());
    }
    game.runner_ref = provisioned.runner_ref;
    game.data_dir = provisioned.data_dir;
    // Saved now, so installing again after a failure finds the prefix set up. Provisioning it again
    // would wait forever on services the app installed (Autodesk's licensing).
    if (auto saved = games.Upsert(game); !saved) return std::unexpected(saved.error());
  }

  WriteDefaults(config, launcher, game.data_dir);
  auto exe = FindExe(launcher, game.data_dir);  // already there: just register it
  if (!exe) {
    int trick = 0;
    for (const std::string& verb : launcher.tricks) {
      Say(launcher, std::format("winetricks {} ({}/{})", verb, ++trick, launcher.tricks.size()));
      const auto on_output = [&launcher](std::string_view chunk) {
        loghub::Append(LogChannel(launcher), chunk);
      };
      if (auto tricked = runner::RunTricksVerb(runners, game, verb, on_output); !tricked)
        return std::unexpected(tricked.error());
    }
    if (launcher.needs_mshtml) {
      Say(launcher, "Registering mshtml");
      if (auto registered = RegisterMshtml(runners, game); !registered)
        return std::unexpected(registered.error());
    }
    if (launcher.id == "adobe") {
      Say(launcher, "Writing Creative Cloud's settings");
      if (auto prepared = adobewine::Prepare(runners, game, paths::UserDir() / "downloads");
          !prepared) {
        return std::unexpected(prepared.error());
      }
    }
    if (launcher.id == "office") {
      Say(launcher, "Writing Microsoft 365's settings and installing its compatibility shims");
      if (auto prepared = office::Prepare(config, runners, game, paths::UserDir() / "downloads");
          !prepared) {
        return std::unexpected(prepared.error());
      }
    }
    for (const Setup& step : launcher.setups) {
      if (!step.done.empty() && FindFile(game.data_dir, step.done))
        continue;  // left from an earlier try
      if (auto ran = RunInstaller(config, launcher, step, game, on_progress); !ran)
        return std::unexpected(ran.error());
    }
    if (!launcher.autodesk_code.empty()) {
      if (auto ran = InstallAutodesk(config, launcher, game, installer, on_progress); !ran) {
        return std::unexpected(ran.error());
      }
    } else if (!launcher.installer.empty()) {
      Setup own;
      own.file = installer;
      if (auto ran = RunInstaller(config, launcher, own, game, on_progress); !ran)
        return std::unexpected(ran.error());
    }
    exe = FindExe(launcher, game.data_dir);
  }
  if (!exe)
    return Err("install_incomplete", std::format("{} didn't finish installing", launcher.name));
  game.install_path = exe->parent_path().string();
  game.exe_path = exe->filename().string();
  game.status = model::GameStatus::Ready;
  return game;
}

}  // namespace

std::string WithoutNoise(std::string_view text) {
  // Messages that mean nothing is wrong: umu marks the mount that holds the install as a Steam
  // library and Proton says it is not one; Office's telemetry helper cannot load a Windows assembly
  // under Wine (it prints a few lines every few seconds).
  constexpr std::array kNoise = {
      std::string_view("unable to use parent for game drive"),
      std::string_view("'Windows, Version=255.255.255.255"),
      std::string_view("InspectorOfficeGadget"), std::string_view("xpdAgent.Log:telemetryService"),
      // Wine's own at every start: no Bluetooth driver, and the touch keyboard's UI hook.
      std::string_view("Services\\winebth"), std::string_view("err:tabtip:"),
      std::string_view("57865755-6c05-4522-98df-4ca658b768ef")};
  const auto noise = [&](std::string_view line) {
    return std::ranges::any_of(kNoise, [&](std::string_view part) { return line.contains(part); });
  };
  std::vector<std::string_view> lines;
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = text.find('\n', at);
    lines.push_back(
        text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at + 1));
    at = end == std::string_view::npos ? text.size() : end + 1;
  }
  std::string kept;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const bool heading =
        lines[i] == "Unhandled Exception:\n" || lines[i] == "Unhandled Exception:\r\n";
    if (noise(lines[i])) continue;
    if (heading && i + 1 < lines.size() && noise(lines[i + 1]))
      continue;  // the heading of a noise block
    kept += lines[i];
  }
  return kept;
}

const std::vector<Launcher>& All() {
  static const std::vector<Launcher> kLaunchers = [] {
    // The Edge WebView2 runtime, which sign-in windows and installers draw with.
    const Setup kWebView2{
        "https://go.microsoft.com/fwlink/?linkid=2124701",
        "webview2-setup.exe",
        {"/silent", "/install"},
        "Program Files (x86)/Microsoft/EdgeWebView/Application/msedgewebview2.exe",
        nullptr,
        {},
        {},
        true};

    Launcher battlenet;
    battlenet.id = "battlenet";
    battlenet.name = "Battle.net";
    battlenet.umu_store = "battlenet";
    battlenet.exe = "Program Files (x86)/Battle.net/Battle.net Launcher.exe";
    battlenet.setups = {
        {"https://downloader.battle.net/download/"
         "getInstaller?os=win&installer=Battle.net-Setup.exe",
         "battlenet-setup.exe",
         {"--lang=enUS", "--installpath=C:\\Program Files (x86)\\Battle.net"},
         "",
         nullptr}};
    battlenet.interactive = true;
    battlenet.env = {{"WINEDLLOVERRIDES", "locationapi=d"}, {"WINE_SIMULATE_WRITECOPY", "1"}};

    Launcher ubisoft;
    ubisoft.id = "ubisoft";
    ubisoft.name = "Ubisoft Connect";
    ubisoft.umu_store = "ubisoft";
    ubisoft.exe = "Program Files (x86)/Ubisoft/Ubisoft Game Launcher/UbisoftConnect.exe";
    ubisoft.tricks = {"d3dcompiler_43", "corefonts", "ubisoftconnect"};

    Launcher ea;
    ea.id = "ea";
    ea.name = "EA app";
    ea.umu_store = "ea";
    ea.exe = "Program Files/Electronic Arts/EA Desktop/EA Desktop/EALauncher.exe";
    ea.setups = {
        {"https://origin-a.akamaihd.net/EA-Desktop-Client-Download/installer-releases/"
         "EAappInstaller.exe",
         "ea-setup.exe",
         {"/silent"},
         "",
         nullptr}};
    ea.tricks = {"corefonts", "d3dcompiler_47"};
    ea.env = {{"LC_ALL", "en_US.UTF-8"}};

    Launcher m365;
    m365.id = "office";
    m365.name = "Microsoft 365";
    m365.kind = "apps";
    // Set up is the prefix and the Edge WebView2 runtime Office signs in with; each app is then
    // installed on its own through SetOfficeApps.
    m365.exe = kWebView2.done;
    m365.tricks = {"corefonts", "msxml6", "riched20", "gdiplus"};
    m365.setups = {kWebView2};
    // Office presents with sync interval 0; under DXVK that tears into flicker.
    m365.env = {{"PROTON_USE_XALIA", "0"}, {"DXVK_CONFIG", "dxgi.syncInterval = 1"}};
    Launcher adobe;
    adobe.id = "adobe";
    adobe.name = "Adobe Creative Cloud";
    adobe.kind = "apps";
    // Not the 32-bit stub in Program Files (x86), which only starts this one and exits: Mira tracks
    // what runs.
    adobe.exe = "Program Files/Adobe/Adobe Creative Cloud/ACC/Creative Cloud.exe";
    // On AdobeWine, set up as its own prefix script does: real fonts (Creative Cloud's renderer
    // crashes without them), the shader compiler Audition uses, DXVK and vkd3d-proton.
    adobe.tricks = {"corefonts", "tahoma", "d3dcompiler_47", "dxvk", "vkd3d"};
    // Creative Cloud's helpers write to heap blocks after freeing them; AdobeWine keeps those out
    // of reuse.
    adobe.env = {
        {"WINE_HEAP_QUARANTINE",
         "Creative Cloud.exe;Creative Cloud Helper.exe;Creative Cloud UI Helper.exe;Adobe Desktop "
         "Service.exe;"
         "node.exe;CCXProcess.exe;CoreSync.exe;CRLogTransport.exe;HDHelper.exe;AdobeIPCBroker.exe;"
         "AdobeUpdateService.exe"}};
    // Its installer signs in on an mshtml page, so it needs mshtml registered and Wine Gecko, which
    // prefixes are made without.
    const Setup kGecko64{
        "https://dl.winehq.org/wine/wine-gecko/2.47.4/wine-gecko-2.47.4-x86_64.msi",
        "wine-gecko-2.47.4-x86_64.msi",
        {"/quiet"},
        "windows/system32/gecko/2.47.4/wine_gecko/xul.dll",
        nullptr};
    const Setup kGecko32{"https://dl.winehq.org/wine/wine-gecko/2.47.4/wine-gecko-2.47.4-x86.msi",
                         "wine-gecko-2.47.4-x86.msi",
                         {"/quiet"},
                         "windows/syswow64/gecko/2.47.4/wine_gecko/xul.dll",
                         nullptr};
    adobe.setups = {
        kGecko64,
        kGecko32,
        kWebView2,
        {"https://prod-rel-ffc-ccm.oobesaas.adobe.com/adobe-ffc-external/core/v1/wam/download"
         "?sapCode=KCCC&productName=Creative%20Cloud&os=win",
         "creative-cloud-setup.exe",
         {},
         "",
         nullptr}};
    adobe.interactive = true;
    adobe.stop_ends_session = true;
    adobe.needs_mshtml = true;
    // The Apps source: each app in its own prefix.
    Launcher fusion;
    fusion.id = "fusion";
    fusion.name = "Autodesk Fusion";
    fusion.app = true;
    // Its launcher's folder stays put; the app's own moves to a new hash folder with each update.
    fusion.exe =
        "users/steamuser/AppData/Local/Autodesk/webdeploy/production/"
        "6a0c9611291d45bb9226980209917c3d/"
        "FusionLauncher.exe";
    fusion.tricks = {"corefonts", "msxml6", "gdiplus", "win10"};
    fusion.setups = {
        {"https://dl.appstreaming.autodesk.com/production/installers/"
         "Fusion%20Client%20Downloader.exe",
         "fusion-client-downloader.exe",
         {"--quiet"},
         "",
         nullptr,
         {},
         {},
         true}};
    // An Autodesk desktop app, from the Create Installer of a custom install in the user's Autodesk
    // account.
    const auto autodesk_app = [](std::string id, std::string name, std::string code,
                                 std::string exe) {
      Launcher app;
      app.id = std::move(id);
      app.name = std::move(name);
      app.app = true;
      app.exe = std::move(exe);
      app.tricks = {"corefonts", "msxml6", "gdiplus", "win10"};
      app.installer = std::format(
          "In your Autodesk account, make a custom install of {} and download its "
          "installer (Create_Installer_{}_....exe), then choose it here.",
          app.name, code);
      app.autodesk_code = std::move(code);
      return app;
    };
    const Launcher autocad =
        autodesk_app("autocad", "AutoCAD", "ACD", "Program Files/Autodesk/AutoCAD 2027/acad.exe");
    const Launcher inventor = autodesk_app("inventor", "Autodesk Inventor Professional", "INVPROSA",
                                           "Program Files/Autodesk/Inventor 2027/Bin/Inventor.exe");
    const Launcher max = autodesk_app("3dsmax", "Autodesk 3ds Max", "3DSMAX",
                                      "Program Files/Autodesk/3ds Max 2027/3dsmax.exe");
    Launcher revit = autodesk_app("revit", "Autodesk Revit", "RVT",
                                  "Program Files/Autodesk/Revit 2027/Revit.exe");
    revit.tricks.push_back(
        "powershell_core");  // its PS2EXE steps run their scripts in PowerShell 7
    // Its installer asks for the licence and product numbers, so the user clicks through it.
    Launcher quickbooks;
    quickbooks.id = "quickbooks";
    quickbooks.name = "QuickBooks Desktop";
    quickbooks.app = true;
    quickbooks.interactive = true;
    quickbooks.exe = "Program Files/Intuit/QuickBooks Enterprise Solutions 24.0/QBW.exe";
    quickbooks.tricks = {"corefonts", "msxml6", "gdiplus", "win10"};
    quickbooks.installer =
        "Download the QuickBooks Desktop installer from your Intuit account (QuickBooks*.exe), "
        "then choose it here. Have its licence and product numbers ready.";
    return std::vector<Launcher>{battlenet, ubisoft,  ea,  m365,  adobe,     fusion,
                                 autocad,   inventor, max, revit, quickbooks};
  }();
  return kLaunchers;
}

const Launcher* Find(std::string_view id) {
  const auto& all = All();
  const auto it = std::ranges::find(all, id, &Launcher::id);
  return it == all.end() ? nullptr : &*it;
}

std::string GameId(const Launcher& launcher) {
  return "launcher-" + launcher.id;
}

const Launcher* ForGame(const model::Game& game) {
  return Find(game.source == "launcher" ? game.source_ref : game.source);
}

bool Installed(const store::GameStore& games, const Launcher& launcher) {
  const auto game = games.Find(GameId(launcher));
  return game && game->status == model::GameStatus::Ready;
}

std::vector<std::string> InstalledOfficeApps(const model::Game& host) {
  std::vector<std::string> apps;
  std::error_code ec;
  for (const office::App& app : office::Apps()) {
    if (fs::is_regular_file(fs::path(host.data_dir) / "drive_c" / office::kProgramDir / app.exe,
                            ec))
      apps.emplace_back(app.ref);
  }
  return apps;
}

Result<void> SetOfficeApps(config::Config& config, store::GameStore& games, api::EventBus& events,
                           std::vector<std::string> apps) {
  const Launcher& launcher = *Find("office");
  const auto host = games.Find(GameId(launcher));
  if (!host || host->status != model::GameStatus::Ready)
    return LauncherNotInstalled(launcher.id, launcher.name);
  if (!BeginInstall(launcher))
    return Err("install_running", "Microsoft 365's installer is already running");
  loghub::Begin(LogChannel(launcher));
  const fs::path downloads = paths::UserDir() / "downloads";
  std::error_code ec;
  fs::create_directories(downloads, ec);
  std::ofstream(downloads / "office-configuration.xml") << office::Configuration(config, apps);
  const Setup step{"https://officecdn.microsoft.com/pr/wsus/setup.exe",
                   std::string(office::kSetupFile),
                   {"/configure", "{downloads}\\office-configuration.xml"},
                   "",
                   office::InstallPercent,
                   office::InstallBytes,
                   office::DownloadProgress,
                   true};
  // Done once exactly the wanted apps are there.
  const auto is_done = [&] {
    std::vector<std::string> now = InstalledOfficeApps(*host);
    std::ranges::sort(now);
    std::ranges::sort(apps);
    return now == apps;
  };
  Result<void> done = RunInstaller(config, launcher, step, *host, {}, is_done);
  if (done) {
    if (auto imported = Import(config, games, events, launcher); !imported)
      done = std::unexpected(imported.error());
  }
  Say(launcher, done ? "Done." : "Failed: " + done.error().message);
  loghub::End(LogChannel(launcher));
  SetState(launcher, done ? "finished" : "failed");
  return done;
}

bool QueueOfficeApps(const std::vector<std::string>& apps) {
  const std::lock_guard lock(state_mutex);
  if (states["office"] != "running") return false;
  for (const std::string& app : apps) {
    if (!std::ranges::contains(queued_office_apps, app)) queued_office_apps.push_back(app);
  }
  return true;
}

void DropQueuedOfficeApps() {
  TakeQueuedOfficeApps();
}

Result<void> AddOfficeApps(config::Config& config, store::GameStore& games, api::EventBus& events,
                           std::vector<std::string> apps) {
  Result<void> done;
  for (std::vector<std::string> more = TakeQueuedOfficeApps();; more = TakeQueuedOfficeApps()) {
    for (std::string& app : more) {
      if (!std::ranges::contains(apps, app)) apps.push_back(std::move(app));
    }
    if (apps.empty() || !done) return done;
    const auto host = games.Find(GameId(*Find("office")));
    std::vector<std::string> wanted =
        host ? InstalledOfficeApps(*host) : std::vector<std::string>{};
    for (std::string& app : apps) {
      if (!std::ranges::contains(wanted, app)) wanted.push_back(std::move(app));
    }
    apps.clear();
    done = SetOfficeApps(config, games, events, std::move(wanted));
  }
}

bool BeginInstall(const Launcher& launcher) {
  const std::lock_guard lock(state_mutex);
  if (states[launcher.id] == "running") return false;
  states[launcher.id] = "running";
  return true;
}

std::string InstallState(const Launcher& launcher) {
  const std::lock_guard lock(state_mutex);
  const auto it = states.find(launcher.id);
  return it == states.end() ? "idle" : it->second;
}

Result<model::Game> Install(config::Config& config, store::GameStore& games,
                            const Launcher& launcher,
                            const std::function<void(double)>& on_progress,
                            const std::string& installer) {
  loghub::Begin(LogChannel(launcher));
  Say(launcher, std::format("Installing {}", launcher.name));
  const Result<model::Game> done = InstallInto(config, games, launcher, on_progress, installer);
  Say(launcher, done ? "Done." : "Failed: " + done.error().message);
  loghub::End(LogChannel(launcher));
  SetState(launcher, done ? "finished" : "failed");
  auto saved = games.Update(GameId(launcher), [&](model::Game& stored) {
    if (done) {
      stored = *done;
    } else {
      stored.status = model::GameStatus::Broken;
      stored.last_error = done.error().message;
    }
    stored.updated_at = model::NowSeconds();
  });
  if (!done) {
    log::Warn("{} install failed: {}", launcher.name, done.error().message);
    return std::unexpected(done.error());
  }
  if (!saved) return std::unexpected(saved.error());
  return *saved;
}

Result<Command> BuildCommand(config::Config& config, const store::GameStore& games,
                             const model::Game& game, std::string_view action) {
  const Launcher* launcher = ForGame(game);
  if (!launcher) return Err("not_launcher_game", "not a store launcher game");
  // The open route passes a bare launcher target with no id; look the host up then.
  const auto host = game.source == "launcher" && !game.id.empty() ? std::optional(game)
                                                                  : games.Find(GameId(*launcher));
  if (!host || host->status != model::GameStatus::Ready) {
    return LauncherNotInstalled(launcher->id, launcher->name);
  }

  const runner::RunnerRegistry runners(config);
  const auto resolved = runners.Resolve(runners.ResolveRef(*host));
  if (!resolved) return std::unexpected(resolved.error());
  model::Game run_as = *host;
  // The EA app moves into a new version folder when it updates.
  std::error_code ec;
  if (!fs::exists(fs::path(host->install_path) / host->exe_path, ec)) {
    if (const auto exe = FindExe(*launcher, host->data_dir)) {
      run_as.install_path = exe->parent_path().string();
      run_as.exe_path = exe->filename().string();
    }
  }
  run_as.args.clear();
  // An app (Word, Photoshop) is its own program in the shared prefix.
  if (launcher->kind == "apps" && game.source != "launcher" && !game.exe_path.empty()) {
    run_as.install_path = game.install_path;
    run_as.exe_path = game.exe_path;
    // Proton's default verb waits for the prefix's running apps to exit first, so a second app
    // wouldn't start until the first closed.
    run_as.env["PROTON_VERB"] = "run";
  }
  // The game's own id, so its window is its own app rather than the launcher's.
  run_as.id = game.id;
  if (const auto gameid = game.runner_config.find("gameid"); gameid != game.runner_config.end()) {
    run_as.runner_config["gameid"] = *gameid;
  } else {
    run_as.runner_config.erase("gameid");
  }
  for (const auto& [key, value] : game.env) run_as.env[key] = value;
  auto command = resolved->runner->BuildCommand(run_as, resolved->build);
  if (!command) return std::unexpected(command.error());

  if (game.source != "launcher") {
    const std::string& ref = game.source_ref;
    const bool install = action == "install";
    if (launcher->id == "battlenet") {
      command->argv.push_back(std::format("--exec={} {}", install ? "install" : "launch", ref));
    } else if (launcher->id == "ubisoft") {
      command->argv.push_back(install ? std::format("uplay://install/{}", ref)
                                      : std::format("uplay://launch/{}/0", ref));
    } else if (launcher->id == "ea") {
      command->argv.push_back(std::format("origin2://game/{}?offerIds={}&autoDownload=1",
                                          install ? "download" : "launch", ref));
    }
  }
  if (launcher->id == "adobe") {
    adobewine::UseIeSignIn(host->data_dir);
    // Creative Cloud draws with Chromium, where with GPU vsync a page change can stay off screen
    // until a repaint.
    if (game.source == "launcher") command->argv.push_back("--disable-gpu-vsync");
  }
  return command;
}

std::string WindowsDir(const model::Game& game) {
  const fs::path drive_c = fs::path(game.data_dir) / "drive_c";
  const fs::path rel = fs::path(game.install_path).lexically_relative(drive_c);
  std::string dir = !rel.empty() && *rel.begin() != ".." ? "c:/" + rel.generic_string()
                                                         : "z:" + game.install_path;
  while (dir.ends_with('/')) dir.pop_back();
  return strings::ToLower(dir);
}

std::string TrackedPath(const model::Game& game) {
  const Launcher* launcher = ForGame(game);
  if (launcher && launcher->kind == "apps" && game.source != "launcher" && !game.exe_path.empty()) {
    return WindowsDir(game) + "/" + strings::ToLower(game.exe_path);
  }
  // A launcher's own folder from its definition, also for an entry an older install registered
  // elsewhere.
  if (launcher && game.source == "launcher" && launcher->stop_ends_session) {
    return strings::ToLower("c:/" + fs::path(launcher->exe).parent_path().generic_string());
  }
  return WindowsDir(game);
}

void EndPrefixSession(const std::string& prefix) {
  std::thread([prefix] { StopPrefix(prefix); }).detach();
}

fs::path HostPath(const fs::path& prefix, std::string_view windows_path) {
  if (windows_path.size() < 2 || windows_path[1] != ':') return {};
  const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(windows_path[0])));
  std::string rest(windows_path.substr(2));
  std::ranges::replace(rest, '\\', '/');
  while (rest.starts_with('/')) rest.erase(0, 1);
  while (rest.ends_with('/')) rest.pop_back();

  fs::path base = prefix / "drive_c";
  if (letter != 'c') {
    std::error_code ec;
    base = fs::weakly_canonical(prefix / "dosdevices" / std::format("{}:", letter), ec);
    if (ec) return {};
  }
  return rest.empty() ? base : base / rest;
}

std::map<std::string, RegValues> ReadRegSubkeys(const fs::path& reg_file, std::string_view parent) {
  // .reg files escape backslashes and quotes inside keys and strings.
  const auto unescape = [](std::string_view in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '\\' && i + 1 < in.size()) ++i;
      out.push_back(in[i]);
    }
    return out;
  };
  const std::string prefix = strings::ToLower(parent) + "\\";

  std::map<std::string, RegValues> subkeys;
  std::ifstream in(reg_file);
  std::string line;
  std::string current;
  while (std::getline(in, line)) {
    if (line.starts_with('[')) {
      current.clear();
      const std::size_t close = line.find(']');
      if (close == std::string::npos) continue;
      const std::string key = unescape(std::string_view(line).substr(1, close - 1));
      if (!strings::ToLower(key).starts_with(prefix)) continue;
      const std::string sub = key.substr(prefix.size());
      if (!sub.empty() && sub.find('\\') == std::string::npos) current = sub;
      continue;
    }
    if (current.empty() || !line.starts_with('"')) continue;
    std::size_t end = 1;
    while (end < line.size() && line[end] != '"') end += line[end] == '\\' ? 2 : 1;
    if (end + 1 >= line.size() || line[end + 1] != '=') continue;
    const std::string name = unescape(std::string_view(line).substr(1, end - 1));
    std::string_view value = std::string_view(line).substr(end + 2);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    subkeys[current][name] = unescape(value);
  }
  return subkeys;
}

}  // namespace mira::launchers
