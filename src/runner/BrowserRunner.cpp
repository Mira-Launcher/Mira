#include "runner/BrowserRunner.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

#include "config/Resolver.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

constexpr int kFirstPort = 41000;
constexpr int kPortSpan = 8000;

fs::path BrowserDir(const model::Game& game) {
  return fs::path(game.data_dir) / "browser";
}

// SO_REUSEADDR as httplib binds, so the last session's sockets in TIME_WAIT don't count.
bool PortFree(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  const int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  const bool free = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  ::close(fd);
  return free;
}

// Each path segment percent-encoded, keeping the slashes between them.
std::string UrlPath(const std::string& relative) {
  std::string out;
  for (const char c : fs::path(relative).generic_string()) {
    const auto byte = static_cast<unsigned char>(c);
    const bool plain =
        std::isalnum(byte) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
    out += plain ? std::string(1, c) : std::format("%{:02X}", byte);
  }
  return out;
}

// Firefox's first-run pages, default-browser question and crash restore, off in the game's profile.
void WriteFirefoxPrefs(const fs::path& profile) {
  std::ofstream prefs(profile / "user.js", std::ios::trunc);
  prefs << "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
           "user_pref(\"browser.aboutwelcome.enabled\", false);\n"
           "user_pref(\"browser.startup.homepage_override.mstone\", \"ignore\");\n"
           "user_pref(\"datareporting.policy.dataSubmissionPolicyBypassNotification\", true);\n"
           "user_pref(\"toolkit.telemetry.reportingpolicy.firstRun\", false);\n"
           "user_pref(\"browser.sessionstore.resume_from_crash\", false);\n"
           "user_pref(\"browser.tabs.warnOnClose\", false);\n";
}

}  // namespace

bool IsFirefoxFamily(std::string_view browser) {
  return browser == "firefox" || browser == "librewolf" || browser == "floorp" ||
         browser == "waterfox" || browser == "zen-browser";
}

int WebGamePort(const model::Game& game) {
  int seed = 0;
  const std::string hash = strings::StableHash(game.id);
  std::from_chars(hash.data(), hash.data() + 4, seed, 16);
  return kFirstPort + seed % kPortSpan;
}

std::vector<model::RunnerBuild> BrowserRunner::Discover(const config::Config&) const {
  std::vector<model::RunnerBuild> builds;
  std::istringstream names{std::string(kBrowsers)};
  for (std::string name; names >> name;) {
    const std::optional<std::string> path = FindOnPath(name);
    if (!path) continue;
    builds.push_back(
        {.kind = "browser", .name = name, .path = *path, .version = "", .release = name});
  }
  return builds;
}

Result<void> BrowserRunner::Provision(const model::Game& game,
                                      const std::optional<model::RunnerBuild>& build) const {
  if (game.data_dir.empty()) {
    return Err("no_data_dir", "this game has no folder for its browser data",
               "Set a data directory for the game, where its saves will go.",
               Fix::Game(game.id, "data_dir"));
  }
  std::error_code ec;
  fs::create_directories(BrowserDir(game) / build->name, ec);
  if (ec) {
    return Err("io_error", std::format("couldn't create {}: {}",
                                       (BrowserDir(game) / build->name).string(), ec.message()));
  }
  return {};
}

Result<Command> BrowserRunner::BuildCommand(const model::Game& game,
                                            const std::optional<model::RunnerBuild>& build) const {
  if (game.exe_path.empty()) return NoExecutable(game);
  if (auto ready = Provision(game, build); !ready) return std::unexpected(ready.error());
  const auto mira_run = ResolveSiblingBinary(OwnBinaryDir(), "mira-run");
  if (!mira_run) return Err("mira_run_missing", "mira-run wasn't found next to mirad or on PATH");
  const int port = WebGamePort(game);
  if (!PortFree(port)) {
    return Err("web_port_busy",
               std::format("port {}, where {} keeps its saves, is in use by another program", port,
                           game.name),
               "Close the program using it (or another copy of the game), then start it again.");
  }

  const fs::path profile = BrowserDir(game) / build->name;
  const std::string url = std::format("http://127.0.0.1:{}/{}", port, UrlPath(game.exe_path));
  const bool fullscreen = config::Resolver(config_, game.overrides).GetBool("web.fullscreen");

  Command command;
  command.argv = {*mira_run, "--serve",  game.install_path, "--port", std::to_string(port),
                  "--",      build->path};
  if (IsFirefoxFamily(build->name)) {
    WriteFirefoxPrefs(profile);
    command.argv.insert(command.argv.end(),
                        {"--new-instance", "--no-remote", "--profile", profile.string()});
    if (fullscreen) command.argv.push_back("--kiosk");
  } else {
    command.argv.insert(command.argv.end(),
                        {"--user-data-dir=" + profile.string(), "--app=" + url, "--no-first-run",
                         "--no-default-browser-check", "--password-store=basic"});
    if (fullscreen) command.argv.push_back("--start-fullscreen");
  }
  const fs::path page = fs::path(game.install_path) / game.exe_path;
  ApplyGameLaunch(command, game, page);
  if (IsFirefoxFamily(build->name)) command.argv.push_back(url);  // after its flags
  return command;
}

}  // namespace mira::runner
