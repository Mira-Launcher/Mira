#include "core/Json.h"
#include "epic/EpicImporter.h"

#include <algorithm>

#include <json.hpp>

#include "core/Log.h"
#include "epic/Legendary.h"
#include "library/ImportSummary.h"
#include "library/PrefixNaming.h"
#include "runner/RunnerRegistry.h"

namespace mira::epic {
namespace {
using nlohmann::json;

struct InstalledTitle {
  std::string app_name;
  std::string title;
  std::string install_path;
  std::string executable;
};

std::vector<InstalledTitle> ParseInstalled(const json& parsed) {
  std::vector<InstalledTitle> out;
  if (!parsed.is_array()) return out;
  for (const auto& entry : parsed) {
    InstalledTitle title;
    title.app_name = core::JsonString(entry, "app_name");
    if (title.app_name.empty()) continue;
    title.title = core::JsonString(entry, "title");
    title.install_path = core::JsonString(entry, "install_path");
    title.executable = core::JsonString(entry, "executable");
    out.push_back(std::move(title));
  }
  return out;
}

}  // namespace

EpicImporter::EpicImporter(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<library::ImportSummary> EpicImporter::Import() {
  library::ImportSummary summary;
  if (!config_.GetBool("epic.enabled")) return summary;

  const Result<json> installed_json = RunLegendaryJson(config_, {"list-installed"});
  if (!installed_json) return std::unexpected(installed_json.error());

  const runner::RunnerRegistry provisioner(config_);

  for (const InstalledTitle& title : ParseInstalled(*installed_json)) {
    const std::string id = "epic-" + title.app_name;
    const auto existing = games_.Find(id);

    // Preserve anything the user already configured across a re-import,
    // only the fields Legendary itself owns get overwritten, same contract
    // as SteamScanner::Scan/LutrisImporter::Import.
    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.source = "epic";
    game.source_ref = title.app_name;
    game.name = title.title.empty() ? title.app_name : title.title;
    game.install_path = title.install_path;
    game.exe_path = title.executable;
    game.platform = model::Platform::Windows;
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    // Legendary makes no prefix of its own. Provisioned on first sight or
    // after a failed attempt, not on every re-import.
    library::ProvisionOnImport(game, existing, config_, games_, provisioner);

    auto result = games_.Merge(existing, game);
    if (!result) {
      log::Error("failed to save epic game {}: {}", id, result.error().message);
      continue;
    }
    library::RecordImported(summary, events_, game, existing.has_value());
  }

  // Titles the account owns but hasn't installed are deliberately NOT
  // upserted here: an entitlement isn't a tracked game, and persisting all
  // of them turned games.toml into 120 rows of placeholders carrying a
  // meaningless data_dir/runner_ref/play_seconds each. GET /v1/library lists
  // them instead; see library::CatalogCache.
  return summary;
}

}  // namespace mira::epic
