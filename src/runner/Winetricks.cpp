#include "runner/Winetricks.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <json.hpp>
#include <optional>
#include <string_view>

#include "core/Command.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/WinePrefix.h"
#include "runner/Curl.h"
#include "runner/Exec.h"
#include "steam/SteamDetector.h"
#include "system/Packages.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

fs::path BundledWinetricks() {
  return paths::UserDir() / "tools" / "winetricks" / "winetricks";
}

// The Visual C++ 14 runtime's least minor version a vcrun verb installs.
std::optional<int> VcRuntimeMinor(std::string_view verb) {
  if (verb == "vcrun2015") return 0;
  if (verb == "vcrun2017") return 10;
  if (verb == "vcrun2019") return 20;
  if (verb == "vcrun2022") return 30;
  return std::nullopt;
}

// Steam installs a game's Visual C++ runtime as a common redistributable, and the verb's
// installer then refuses to put an older one over it, which aborts winetricks.
bool HasVcRuntime(const fs::path& prefix, int minor) {
  constexpr std::string_view kKey = "Software\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes";
  constexpr std::string_view kWow =
      "Software\\Wow6432Node\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes";
  const auto has = [&](std::string_view parent, std::string_view arch) {
    for (const auto& [name, values] : library::ReadRegSubkeys(prefix / "system.reg", parent)) {
      if (!strings::EqualsIgnoreCase(name, arch)) continue;
      const auto dword = [&values](const char* key) {
        const auto it = values.find(key);
        if (it == values.end() || !it->second.starts_with("dword:")) return -1L;
        return std::strtol(it->second.c_str() + 6, nullptr, 16);
      };
      return dword("Installed") == 1 && dword("Major") == 14 && dword("Minor") >= minor;
    }
    return false;
  };
  std::error_code ec;
  // A 32-bit prefix (no syswow64) has only the x86 runtime, in the plain key.
  if (!fs::is_directory(prefix / "drive_c" / "windows" / "syswow64", ec)) return has(kKey, "x86");
  return has(kKey, "x64") && has(kWow, "x86");
}

}  // namespace

Result<fs::path> ResolveWineBinary(const RunnerRegistry& runners, const model::Game& game) {
  const std::string ref = game.runner_ref.empty() ? "native:native" : game.runner_ref;
  const auto resolved = runners.Resolve(ref);
  if (!resolved) return std::unexpected(resolved.error());

  const std::string kind = resolved->runner->kind();
  if (kind == "wine") {
    if (!resolved->build) return NoBuild("Wine");
    return fs::path(resolved->build->path);
  }
  if (kind == "proton") {
    if (!resolved->build) return NoBuild("Proton");
    return fs::path(resolved->build->path) / "files" / "bin" / "wine";
  }
  if (kind == "steam") {
    if (game.data_dir.empty()) {
      return Err("not_windows",
                 "this Steam game is native, and winetricks needs a Wine/Proton prefix");
    }
    const auto info = steam::ResolveProtonCompatInfo(game.data_dir);
    if (!info) {
      return Err("steam_proton_unresolved",
                 "couldn't determine which Proton build this prefix uses. Run this game once "
                 "through Steam first");
    }
    return info->proton_path.parent_path() / "files" / "bin" / "wine";
  }
  return Err("not_wine_based",
             std::format("winetricks only applies to a Wine or Proton runner, not \"{}\"", kind));
}

std::string WinetricksPath() {
  if (auto found = FindOnPath("winetricks")) return *found;
  std::error_code ec;
  return fs::exists(BundledWinetricks(), ec) ? BundledWinetricks().string() : std::string();
}

Result<std::string> LatestWinetricksTag() {
  const Result<nlohmann::json> listed =
      CurlJson("https://api.github.com/repos/Winetricks/winetricks/releases/latest");
  if (!listed) return std::unexpected(listed.error());
  const nlohmann::json& release = *listed;
  const std::string tag =
      release.is_object() ? release.value("tag_name", std::string()) : std::string();
  if (tag.empty())
    return Err("github_api_error", "couldn't find the latest winetricks release", kGitHubHint);
  return tag;
}

std::string InstalledWinetricksVersion(const fs::path& script) {
  constexpr std::string_view kPrefix = "WINETRICKS_VERSION=";
  std::ifstream file(script);
  for (std::string line; std::getline(file, line);) {
    if (line.starts_with(kPrefix)) return line.substr(kPrefix.size());
  }
  return {};
}

// Its releases ship no script asset, so fetch the script at the latest tag.
Result<void> InstallWinetricks() {
  const Result<std::string> latest = LatestWinetricksTag();
  if (!latest) return std::unexpected(latest.error());
  const std::string& tag = *latest;

  const fs::path target = BundledWinetricks();
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec) return Err("install_dir_failed", ec.message());
  if (auto downloaded = CurlDownload(
          std::format("https://raw.githubusercontent.com/Winetricks/winetricks/{}/src/winetricks",
                      tag),
          target);
      !downloaded) {
    return downloaded;
  }
  fs::permissions(target, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                  fs::perm_options::add, ec);
  if (ec) return Err("chmod_failed", ec.message());
  return {};
}

Result<void> RunTricksVerb(const RunnerRegistry& runners, const model::Game& game,
                           const std::string& verb, const OutputFn& on_output) {
  if (game.data_dir.empty()) return NoPrefix(game);
  // A Steam game's data_dir is its compatdata folder, which holds the prefix as pfx/.
  const fs::path prefix = library::PrefixRoot(game.data_dir);
  std::error_code ec;
  if (!fs::exists(prefix / "drive_c", ec)) {
    return Err("not_provisioned", "this game's Wine prefix hasn't been created yet",
               "Launch the game once, or run something in its prefix, to create it.");
  }

  const Result<fs::path> wine_binary = ResolveWineBinary(runners, game);
  if (!wine_binary) return std::unexpected(wine_binary.error());

  const std::string winetricks = WinetricksPath();
  if (winetricks.empty()) {
    return Err("winetricks_missing", "winetricks isn't installed", "Install winetricks.",
               Fix::Runners("winetricks"));
  }
  if (auto ready = system::RequireFeature("winetricks", "winetricks"); !ready) return ready;

  if (verb.empty() || verb.front() == '-' || !std::ranges::all_of(verb, [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '.' || c == '=' || c == '-';
      })) {
    return Err("invalid_verb", "that isn't a winetricks verb");
  }
  if (const auto minor = VcRuntimeMinor(verb); minor && HasVcRuntime(prefix, *minor)) {
    if (on_output)
      on_output(
          std::format("{}: this Visual C++ runtime or a newer one is already installed\n", verb));
    return {};
  }
  const auto prefix_lock = LockPrefix(game.data_dir);

  Command command;
  command.argv = {winetricks, "--unattended", verb};
  command.env["WINE"] = wine_binary->string();
  command.env["WINEPREFIX"] = prefix.string();
  const fs::path wineserver = wine_binary->parent_path() / "wineserver";
  if (fs::exists(wineserver, ec)) command.env["WINESERVER"] = wineserver.string();

  const Result<ExecResult> result = RunAndWait(command, on_output);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("tricks_failed",
               std::format("winetricks {} exited {}: {}", verb, result->exit_code, result->output));
  }
  return {};
}

Result<ExecResult> RunWine(const RunnerRegistry& runners, const model::Game& game,
                           const std::vector<std::string>& args) {
  if (game.data_dir.empty()) return NoPrefix(game);
  const Result<fs::path> wine_binary = ResolveWineBinary(runners, game);
  if (!wine_binary) return std::unexpected(wine_binary.error());
  const auto prefix_lock = LockPrefix(game.data_dir);
  Command command;
  command.argv = {wine_binary->string()};
  command.argv.insert(command.argv.end(), args.begin(), args.end());
  command.env["WINEPREFIX"] = library::PrefixRoot(game.data_dir).string();
  command.env["WINEDEBUG"] = "-all";
  return RunAndWait(command);
}

std::vector<std::string> InstalledVerbs(const model::Game& game) {
  std::vector<std::string> verbs;
  if (game.data_dir.empty()) return verbs;
  std::ifstream log(library::PrefixRoot(game.data_dir) / "winetricks.log");
  for (std::string line; std::getline(log, line);) {
    // A forced rerun logs the verb again, and some lines are winetricks' own options.
    if (line.empty() || line.starts_with('-') || std::ranges::contains(verbs, line)) continue;
    verbs.push_back(line);
  }
  return verbs;
}

}  // namespace mira::runner
