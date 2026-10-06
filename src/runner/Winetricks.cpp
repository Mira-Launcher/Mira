#include "runner/Winetricks.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

#include <json.hpp>

#include "core/Command.h"
#include "core/Paths.h"
#include "runner/Curl.h"
#include "runner/Exec.h"
#include "steam/SteamDetector.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

fs::path BundledWinetricks() { return paths::UserDir() / "tools" / "winetricks" / "winetricks"; }

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
      return Err("not_windows", "this Steam game is native, and winetricks needs a Wine/Proton prefix");
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

// Its releases ship no script asset, so fetch the script at the latest tag.
Result<void> InstallWinetricks() {
  const Result<nlohmann::json> listed = CurlJson("https://api.github.com/repos/Winetricks/winetricks/releases/latest");
  if (!listed) return std::unexpected(listed.error());
  const nlohmann::json& release = *listed;
  const std::string tag = release.is_object() ? release.value("tag_name", std::string()) : std::string();
  if (tag.empty()) return Err("github_api_error", "couldn't find the latest winetricks release", kConnectionHint);

  const fs::path target = BundledWinetricks();
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec) return Err("install_dir_failed", ec.message());
  if (auto downloaded = CurlDownload(
          std::format("https://raw.githubusercontent.com/Winetricks/winetricks/{}/src/winetricks", tag), target);
      !downloaded) {
    return downloaded;
  }
  fs::permissions(target, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                  fs::perm_options::add, ec);
  if (ec) return Err("chmod_failed", ec.message());
  return {};
}

Result<void> RunTricksVerb(const RunnerRegistry& runners, const model::Game& game, const std::string& verb,
                           const OutputFn& on_output) {
  if (game.data_dir.empty()) return NoPrefix(game);
  std::error_code ec;
  if (!fs::exists(fs::path(game.data_dir) / "drive_c", ec)) {
    return Err("not_provisioned", "this game's Wine prefix hasn't been created yet",
               "Launch the game once, or run something in its prefix, to create it.");
  }

  const Result<fs::path> wine_binary = ResolveWineBinary(runners, game);
  if (!wine_binary) return std::unexpected(wine_binary.error());

  const std::string winetricks = WinetricksPath();
  if (winetricks.empty()) {
    return Err("winetricks_missing", "winetricks isn't installed", "Install winetricks.", Fix::Runners("winetricks"));
  }

  if (verb.empty() || verb.front() == '-' ||
      !std::ranges::all_of(verb, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '=' || c == '-'; })) {
    return Err("invalid_verb", "that isn't a winetricks verb");
  }
  const auto prefix_lock = LockPrefix(game.data_dir);

  Command command;
  command.argv = {winetricks, "--unattended", verb};
  command.env["WINE"] = wine_binary->string();
  command.env["WINEPREFIX"] = game.data_dir;
  const fs::path wineserver = wine_binary->parent_path() / "wineserver";
  if (fs::exists(wineserver, ec)) command.env["WINESERVER"] = wineserver.string();

  const Result<ExecResult> result = RunAndWait(command, on_output);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("tricks_failed", std::format("winetricks {} exited {}: {}", verb, result->exit_code, result->output));
  }
  return {};
}

Result<ExecResult> RunWine(const RunnerRegistry& runners, const model::Game& game, const std::vector<std::string>& args) {
  if (game.data_dir.empty()) return NoPrefix(game);
  const Result<fs::path> wine_binary = ResolveWineBinary(runners, game);
  if (!wine_binary) return std::unexpected(wine_binary.error());
  const auto prefix_lock = LockPrefix(game.data_dir);
  Command command;
  command.argv = {wine_binary->string()};
  command.argv.insert(command.argv.end(), args.begin(), args.end());
  command.env["WINEPREFIX"] = game.data_dir;
  command.env["WINEDEBUG"] = "-all";
  return RunAndWait(command);
}

}  // namespace mira::runner
