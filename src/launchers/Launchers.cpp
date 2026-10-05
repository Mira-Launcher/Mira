#include "launchers/Launchers.h"

#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

#include "core/Lane.h"
#include "core/Log.h"
#include "core/StoreErrors.h"
#include "proc/ProcessSupervisor.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/PrefixNaming.h"
#include "runner/Curl.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"
#include "runner/Winetricks.h"

namespace mira::launchers {
namespace {
namespace fs = std::filesystem;

std::mutex state_mutex;
std::map<std::string, std::string> states;  // launcher id -> running, finished, failed

void SetState(const Launcher& launcher, std::string state) {
  const std::lock_guard lock(state_mutex);
  states[launcher.id] = std::move(state);
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

// The launcher exe, or the same file name anywhere under its top folder
// (the EA app sometimes nests it under a version folder).
std::optional<fs::path> FindExe(const Launcher& launcher, const fs::path& prefix) {
  const fs::path expected = prefix / "drive_c" / launcher.exe;
  std::error_code ec;
  if (fs::is_regular_file(expected, ec)) return expected;
  const fs::path rel(launcher.exe);
  const fs::path top = prefix / "drive_c" / *rel.begin() / *std::next(rel.begin());
  for (fs::recursive_directory_iterator it(top, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->path().filename() == rel.filename() && it->is_regular_file(ec)) return it->path();
  }
  return std::nullopt;
}

Result<void> RunInstaller(config::Config& config, const Launcher& launcher, const model::Game& game) {
  ReapOrphans();
  const fs::path downloads = paths::UserDir() / "downloads";
  const fs::path setup = downloads / std::format("{}-setup.exe", launcher.id);
  std::error_code ec;
  fs::create_directories(downloads, ec);

  if (auto fetched = runner::CurlDownload(launcher.installer_url, setup); !fetched) {
    return Err("download_failed", std::format("couldn't download the {} installer: {}", launcher.name,
                                              fetched.error().message), kConnectionHint);
  }

  const runner::RunnerRegistry runners(config);
  const auto resolved = runners.Resolve(game.runner_ref);
  if (!resolved) return std::unexpected(resolved.error());
  model::Game run_as = game;
  run_as.install_path = downloads.string();
  run_as.exe_path = setup.filename().string();
  run_as.args.clear();
  auto command = resolved->runner->BuildCommand(run_as, resolved->build);
  if (!command) return std::unexpected(command.error());
  command->argv.insert(command->argv.end(), launcher.installer_args.begin(), launcher.installer_args.end());

  log::Info("running the {} installer", launcher.name);
  const auto pid = runner::SpawnDetached(*command);
  if (!pid) return std::unexpected(pid.error());
  // umu-run only exits once everything in the prefix has, and a freshly
  // installed launcher (and the EA app's background service) keeps
  // running. So wait for the setup process itself instead. Exit codes
  // aren't reliable either (EA's returns 768 on success); whether the
  // launcher exe exists afterwards decides.
  const std::string setup_dir = strings::ToLower("z:" + downloads.string());
  // Battle.net's setup hands off to a second stage, so the exe must exist too.
  while (::waitpid(*pid, nullptr, WNOHANG) != *pid) {
    if (proc::FindDirProcesses(game.data_dir, setup_dir).empty() && FindExe(launcher, game.data_dir)) {
      ReapOrphans(*pid);
      return {};
    }
    if (ThisTaskStop().stop_requested()) {
      ReapOrphans(*pid);
      return Err("shutting_down", "mirad stopped before the installer finished");
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  if (!FindExe(launcher, game.data_dir)) {
    return Err("launcher_not_installed", std::format("the {} installer finished but its launcher wasn't found", launcher.name));
  }
  return {};
}

Result<model::Game> InstallInto(config::Config& config, store::GameStore& games, const Launcher& launcher) {
  const std::string id = GameId(launcher);
  model::Game game = games.Find(id).value_or(model::Game{});
  const bool existed = !game.id.empty();
  game.id = id;
  game.name = launcher.name;
  game.source = "launcher";
  game.source_ref = launcher.id;
  game.platform = model::Platform::Windows;
  for (const auto& [key, value] : launcher.env) game.env.try_emplace(key, value);
  if (!launcher.umu_store.empty()) game.runner_config["store"] = launcher.umu_store;
  if (game.runner_ref.empty()) game.runner_ref = config.GetString("launchers.runner");
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
    const model::Game provisioned = runners.ProvisionGame(game);
    if (provisioned.status == model::GameStatus::Broken) {
      return Err("provision_failed", "couldn't set up the launcher's Wine prefix: " + provisioned.last_error,
                 "The runner may be broken. Try a different one.", Fix::Runners());
    }
    game.runner_ref = provisioned.runner_ref;
    game.data_dir = provisioned.data_dir;
  }

  WriteDefaults(config, launcher, game.data_dir);
  auto exe = FindExe(launcher, game.data_dir);  // already there: just register it
  if (!exe) {
    for (const std::string& verb : launcher.tricks) {
      log::Info("winetricks {} for {}", verb, launcher.name);
      if (auto tricked = runner::RunTricksVerb(runners, game, verb); !tricked) return std::unexpected(tricked.error());
    }
    if (!launcher.installer_url.empty()) {
      if (auto ran = RunInstaller(config, launcher, game); !ran) return std::unexpected(ran.error());
    }
    exe = FindExe(launcher, game.data_dir);
  }
  if (!exe) return Err("install_incomplete", std::format("{} didn't finish installing", launcher.name));
  game.install_path = exe->parent_path().string();
  game.exe_path = exe->filename().string();
  game.status = model::GameStatus::Ready;
  return game;
}

}  // namespace

const std::vector<Launcher>& All() {
  static const std::vector<Launcher> kLaunchers = [] {
    Launcher battlenet;
    battlenet.id = "battlenet";
    battlenet.name = "Battle.net";
    battlenet.umu_store = "battlenet";
    battlenet.exe = "Program Files (x86)/Battle.net/Battle.net Launcher.exe";
    battlenet.installer_url = "https://downloader.battle.net/download/getInstaller?os=win&installer=Battle.net-Setup.exe";
    battlenet.installer_args = {"--lang=enUS", "--installpath=C:\\Program Files (x86)\\Battle.net"};
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
    ea.installer_url = "https://origin-a.akamaihd.net/EA-Desktop-Client-Download/installer-releases/EAappInstaller.exe";
    ea.installer_args = {"/silent"};
    ea.tricks = {"corefonts", "d3dcompiler_47"};
    ea.env = {{"LC_ALL", "en_US.UTF-8"}};
    return std::vector<Launcher>{battlenet, ubisoft, ea};
  }();
  return kLaunchers;
}

const Launcher* Find(std::string_view id) {
  const auto& all = All();
  const auto it = std::ranges::find(all, id, &Launcher::id);
  return it == all.end() ? nullptr : &*it;
}

std::string GameId(const Launcher& launcher) { return "launcher-" + launcher.id; }

const Launcher* ForGame(const model::Game& game) {
  return Find(game.source == "launcher" ? game.source_ref : game.source);
}

bool Installed(const store::GameStore& games, const Launcher& launcher) {
  const auto game = games.Find(GameId(launcher));
  return game && game->status == model::GameStatus::Ready;
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

Result<model::Game> Install(config::Config& config, store::GameStore& games, const Launcher& launcher) {
  const Result<model::Game> done = InstallInto(config, games, launcher);
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

Result<Command> BuildCommand(config::Config& config, const store::GameStore& games, const model::Game& game,
                             std::string_view action) {
  const Launcher* launcher = ForGame(game);
  if (!launcher) return Err("not_launcher_game", "not a store launcher game");
  const auto host = game.source == "launcher" ? std::optional(game) : games.Find(GameId(*launcher));
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
      command->argv.push_back(install ? std::format("uplay://install/{}", ref) : std::format("uplay://launch/{}/0", ref));
    } else if (launcher->id == "ea") {
      command->argv.push_back(std::format("origin2://game/{}?offerIds={}&autoDownload=1",
                                          install ? "download" : "launch", ref));
    }
  }
  return command;
}

std::string WindowsDir(const model::Game& game) {
  const fs::path drive_c = fs::path(game.data_dir) / "drive_c";
  const fs::path rel = fs::path(game.install_path).lexically_relative(drive_c);
  std::string dir = !rel.empty() && *rel.begin() != ".." ? "c:/" + rel.generic_string() : "z:" + game.install_path;
  while (dir.ends_with('/')) dir.pop_back();
  return strings::ToLower(dir);
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
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    subkeys[current][name] = unescape(value);
  }
  return subkeys;
}

}  // namespace mira::launchers
