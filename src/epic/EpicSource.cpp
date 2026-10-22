#include "epic/EpicSource.h"

#include <algorithm>
#include <format>
#include <json.hpp>

#include "core/Command.h"
#include "core/Json.h"
#include "core/StoreErrors.h"
#include "epic/EpicImporter.h"
#include "epic/EpicInstaller.h"
#include "epic/Legendary.h"

namespace mira::epic {
using nlohmann::json;

Result<std::vector<library::CatalogEntry>> EpicSource::Catalog(
    const config::Config& config, const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!games.SourceEnabled("epic")) return entries;

  const Result<json> listed = RunLegendaryJson(config, {"list"});
  if (!listed) return std::unexpected(listed.error());
  if (!listed->is_array()) return entries;

  for (const json& item : *listed) {
    library::CatalogEntry entry;
    entry.source = "epic";
    entry.ref = core::JsonString(item, "app_name");
    if (entry.ref.empty()) continue;
    // `list --json` calls this "app_title"; `list-installed --json` calls
    // the same thing "title" -- accept either.
    entry.title = core::JsonString(item, "app_title", core::JsonString(item, "title"));
    if (entry.title.empty()) entry.title = entry.ref;
    library::MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

Result<void> EpicSource::Install(config::Config& config, store::GameStore& games,
                                 api::EventBus& events, const std::string& ref) {
  if (auto ready = CheckReady(config); !ready) return ready;
  EpicInstaller installer(config, games, events);
  return installer.Install(ref);
}

Result<void> EpicSource::Update(config::Config& config, store::GameStore& games,
                                api::EventBus& events, const std::string& ref) {
  if (auto ready = CheckReady(config); !ready) return ready;
  EpicInstaller installer(config, games, events);
  return installer.Update(ref);
}

namespace {

// `legendary launch <app> --json`: the parameters it would start the game with, nothing started.
// Its output holds a live exchange code, so it never goes into an error or a log.
Result<json> LaunchParameters(const config::Config& config, const std::string& app_name,
                              bool offline) {
  const runner::ToolStatus tool = DetectLegendary(config);
  if (!tool.installed) return Err("legendary_missing", "Legendary isn't installed");
  Command command;
  // The update check would fetch Epic's whole asset list first.
  command.argv = {tool.path, "launch", app_name, "--json", "--skip-version-check"};
  if (offline) command.argv.push_back("--offline");
  command.timeout_s = 10;
  const auto result = runner::RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("legendary_failed", std::format("legendary launch exited {}", result->exit_code));
  }
  json parsed = core::ParseJsonTail(result->output);
  if (!parsed.is_object())
    return Err("legendary_json_error", "legendary launch gave no parameters");
  return parsed;
}

}  // namespace

Result<library::LaunchExtras> EpicSource::BeforeLaunch(const config::Config& config,
                                                       const model::Game& game,
                                                       bool sign_in) const {
  if (!sign_in || game.source_ref.empty()) return library::LaunchExtras{};
  if (!Status(config).authenticated) return Err("not_authenticated", "Epic isn't signed in");
  auto params = LaunchParameters(config, game.source_ref, false);
  // Offline, the ids still go in; only the sign-in itself is missing.
  if (!params) {
    if (auto offline = LaunchParameters(config, game.source_ref, true)) params = std::move(offline);
  }
  if (!params) return std::unexpected(params.error());
  library::LaunchExtras extras;
  extras.redact = {"-AUTH_PASSWORD="};
  const json egl = params->value("egl_parameters", json::array());
  if (!egl.is_array()) return extras;
  for (const json& value : egl) {
    if (!value.is_string()) continue;
    std::string arg = value.get<std::string>();
    // An ownership token legendary wrote for this launch, read by the game through Wine.
    if (constexpr std::string_view kToken = "-epicovt="; arg.starts_with(kToken)) {
      const std::string file = arg.substr(kToken.size());
      extras.remove_after_exit.emplace_back(file);
      if (game.platform == model::Platform::Windows) {
        std::string windows = "Z:" + file;
        std::ranges::replace(windows, '/', '\\');
        arg = std::string(kToken) + windows;
      }
    }
    extras.args.push_back(std::move(arg));
  }
  return extras;
}

}  // namespace mira::epic
