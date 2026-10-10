#include "core/Json.h"
#include "core/Strings.h"
#include "amazon/AmazonImporter.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>

#include <json.hpp>

#include "amazon/Nile.h"
#include "core/Log.h"
#include "library/ImportSummary.h"
#include "library/PrefixNaming.h"
#include "runner/RunnerRegistry.h"

namespace mira::amazon {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

std::string ForwardSlashes(std::string path) {
  std::ranges::replace(path, '\\', '/');
  return path;
}

}  // namespace

AmazonImporter::AmazonImporter(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<library::ImportSummary> AmazonImporter::Import() {
  library::ImportSummary summary;
  if (!games_.SourceEnabled("amazon")) return summary;
  const json installed = ReadNileFile("installed.json");
  if (!installed.is_array()) return summary;  // nothing installed yet
  const json library = ReadNileFile("library.json");
  std::map<std::string, const json*> owned_by_id;
  if (library.is_array()) {
    for (const json& item : library) {
      if (item.is_object() && item.contains("product")) {
        owned_by_id.try_emplace(core::JsonString(item["product"], "id"), &item);
      }
    }
  }

  for (const json& entry : installed) {
    const std::string product_id = core::JsonString(entry, "id");
    const fs::path path = core::JsonString(entry, "path");
    std::error_code ec;
    if (product_id.empty() || !fs::is_directory(path, ec)) continue;

    static const json kNone = json::object();
    const auto found = owned_by_id.find(product_id);
    const json& owned = found != owned_by_id.end() ? *found->second : kNone;
    const json product = owned.contains("product") ? owned["product"] : json::object();

    // fuel.json is what `nile launch` runs: Main.Command relative to the
    // install, plus Args and an optional working directory.
    std::ifstream fuel_file(path / "fuel.json");
    const json fuel = json::parse(fuel_file, nullptr, false, true);
    const json main = fuel.is_object() && fuel.contains("Main") ? fuel["Main"] : json::object();
    const std::string command = ForwardSlashes(core::JsonString(main, "Command"));

    const std::string id = "amazon-" + product_id;
    const auto existing = games_.Find(id);
    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.source = "amazon";
    game.source_ref = product_id;
    if (product.contains("title") && product["title"].is_string()) {
      game.name = product["title"].get<std::string>();
    } else if (game.name.empty()) {
      game.name = path.filename().string();
    }
    game.install_path = path.string();
    if (!command.empty()) game.exe_path = command;
    if (const json args = main.contains("Args") ? main["Args"] : json::array(); args.is_array() && game.args.empty()) {
      std::vector<std::string> list;
      for (const json& arg : args) {
        if (arg.is_string()) list.push_back(arg.get<std::string>());
      }
      game.args = strings::JoinArgs(list);
    }
    if (main.contains("WorkingSubdirOverride") && main["WorkingSubdirOverride"].is_string()) {
      game.working_dir = ForwardSlashes(main["WorkingSubdirOverride"].get<std::string>());
    }
    game.platform = model::Platform::Windows;
    const fs::path sdk = NileConfigDir() / "SDK" / "Amazon Games Services";
    game.env["FUEL_DIR"] = (sdk / "Legacy").string();
    game.env["AMAZON_GAMES_SDK_PATH"] = (sdk / "AmazonGamesSDK").string();
    game.env["AMAZON_GAMES_FUEL_ENTITLEMENT_ID"] = core::JsonString(owned, "id");
    game.env["AMAZON_GAMES_FUEL_PRODUCT_SKU"] = core::JsonString(product, "sku");
    game.runner_config["store"] = "amazon";
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    if (game.exe_path.empty()) {
      game.status = model::GameStatus::Broken;
      game.last_error = "no fuel.json launch command in this install";
    } else {
      const runner::RunnerRegistry provisioner(config_);
      library::ProvisionOnImport(game, existing, config_, games_, provisioner);
    }

    if (auto saved = games_.Merge(existing, game); !saved) {
      log::Error("failed to import amazon game {}: {}", product_id, saved.error().message);
      continue;
    }
    library::RecordImported(summary, events_, game, existing.has_value());
  }
  return summary;
}

}  // namespace mira::amazon
