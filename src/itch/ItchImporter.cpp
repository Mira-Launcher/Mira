#include "core/Json.h"
#include "itch/ItchImporter.h"

#include <algorithm>

#include <json.hpp>

#include "core/Log.h"
#include "itch/Butlerd.h"
#include "itch/Itch.h"
#include "library/Detector.h"
#include "library/ImportSummary.h"
#include "library/PrefixNaming.h"
#include "runner/RunnerRegistry.h"

namespace mira::itch {
namespace {
using nlohmann::json;

struct Cave {
  std::string cave_id;
  std::string game_id;
  std::string title;
  std::string install_folder;
  bool is_linux_native = false;
};

// Field names confirmed against butlerd's own spec (butlerd/generous/
// spec/butlerd.json in itchio/butler).
std::vector<Cave> ParseCaves(const json& items) {
  std::vector<Cave> out;
  if (!items.is_array()) return out;
  for (const json& item : items) {
    Cave cave;
    cave.cave_id = core::JsonString(item, "id");
    if (cave.cave_id.empty()) continue;

    const json game = item.is_object() && item.contains("game") ? item["game"] : json::object();
    cave.game_id = std::to_string(core::JsonInt(game, "id"));
    cave.title = core::JsonString(game, "title");

    const json install_info = item.contains("installInfo") ? item["installInfo"] : json::object();
    cave.install_folder = core::JsonString(install_info, "installFolder");

    // Upload.platforms.linux is an enum string ("all"/"386"/"amd64") when
    // present, absent otherwise -- not a bool, confirmed against
    // butlerd's own spec (a naive .value<bool>() here throws on a real
    // linux upload).
    const json upload = item.contains("upload") ? item["upload"] : json::object();
    cave.is_linux_native = upload.contains("platforms") && upload["platforms"].is_object() && upload["platforms"].contains("linux");

    out.push_back(std::move(cave));
  }
  return out;
}

}  // namespace

ItchImporter::ItchImporter(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<library::ImportSummary> ItchImporter::Import() {
  library::ImportSummary summary;
  if (!config_.GetBool("itch.enabled")) return summary;

  const Result<std::int64_t> profile_id = CurrentProfileId(config_);
  if (!profile_id) return std::unexpected(profile_id.error());

  const Result<json> caves_json = Call(config_, "Fetch.Caves", {{"profileId", *profile_id}});
  if (!caves_json) return std::unexpected(caves_json.error());

  const json items = caves_json->is_object() && caves_json->contains("items") ? (*caves_json)["items"] : json::array();
  const runner::RunnerRegistry provisioner(config_);

  for (const Cave& cave : ParseCaves(items)) {
    if (cave.game_id.empty() || cave.game_id == "0") continue;
    const std::string id = "itch-" + cave.game_id;
    const auto existing = games_.Find(id);

    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.source = "itch";
    game.source_ref = cave.game_id;
    game.name = cave.title.empty() ? cave.game_id : cave.title;
    game.install_path = cave.install_folder;
    game.platform = cave.is_linux_native ? model::Platform::Native : model::Platform::Windows;
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;
    library::AddTag(game.tags, "itch");

    // butlerd's Cave doesn't say which file to run -- reuse Mira's own
    // executable detector against the real install folder, same as a
    // manually-scanned game. Only when exe_path isn't already set, so a
    // user's own correction survives a re-import.
    if (game.exe_path.empty() && !cave.install_folder.empty()) {
      const library::Detector::Result detected = library::Detector({}).Detect(cave.install_folder);
      if (!detected.candidates.empty()) {
        game.exe_path = detected.candidates.front().rel_path;
        game.candidates = detected.candidates;
        game.confidence = detected.confidence;
      }
    }

    if (game.platform == model::Platform::Windows &&
        library::NeedsProvisioning(existing)) {
      if (game.data_dir.empty()) game.data_dir = library::PrefixDir(config_, games_, game).string();
      const model::Game provisioned = provisioner.ProvisionGame(game);
      game.runner_ref = provisioned.runner_ref;
      game.data_dir = provisioned.data_dir;
      game.status = provisioned.status;
      game.last_error = provisioned.last_error;
    } else {
      game.status = model::GameStatus::Ready;
    }

    auto result = games_.Merge(existing, game);
    if (!result) {
      log::Error("failed to save itch game {}: {}", id, result.error().message);
      continue;
    }
    library::RecordImported(summary, events_, game, existing.has_value());
  }
  return summary;
}

}  // namespace mira::itch
