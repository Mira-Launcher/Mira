#include "gog/GogImporter.h"

#include <algorithm>
#include <fstream>
#include <optional>

#include <json.hpp>

#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "gog/Gog.h"
#include "library/ImportSummary.h"
#include "library/PrefixNaming.h"
#include "runner/RunnerRegistry.h"

namespace mira::gog {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

std::filesystem::path InstallRoot(const config::Config& config) { return config.GetPath("gog.install_root"); }

// GOG's product id, from the goggame-<id>.info every gogdl install carries.
std::optional<std::string> GogIdIn(const fs::path& dir) {
  for (const fs::path& entry : paths::ListDir(dir)) {
    const std::string name = entry.filename().string();
    if (!name.starts_with("goggame-") || !name.ends_with(".info")) continue;
    const std::string id = name.substr(8, name.size() - 8 - 5);
    if (!id.empty() && std::ranges::all_of(id, [](char c) { return c >= '0' && c <= '9'; })) return id;
  }
  return std::nullopt;
}

// The game directory itself when it holds a goggame-*.info; otherwise the
// legacy <install_root>/<id>/<Title>/ layout's single subdirectory, or `dir`.
fs::path ResolveGameDir(const fs::path& dir) {
  if (GogIdIn(dir)) return dir;
  std::error_code ec;
  fs::path only;
  int count = 0;
  for (const fs::path& entry : paths::ListDir(dir)) {
    if (!fs::is_directory(entry, ec)) continue;
    only = entry;
    if (++count > 1) break;
  }
  return count == 1 ? only : dir;
}

}  // namespace

std::filesystem::path FindGameDir(const config::Config& config, const std::string& id) {
  std::error_code ec;
  for (const fs::path& entry : paths::ListDir(InstallRoot(config))) {
    if (!fs::is_directory(entry, ec)) continue;
    const fs::path dir = ResolveGameDir(entry);
    if (GogIdIn(dir) == id) return dir;
  }
  return {};
}

GogImporter::GogImporter(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<model::Game> GogImporter::ImportPath(const std::string& id, const std::filesystem::path& path, bool refresh) {
  const std::string game_id = "gog-" + id;
  const auto existing = games_.Find(game_id);
  const fs::path game_dir = ResolveGameDir(path);

  // gogdl's own `import <path>` output only confirms the install and
  // names/points at it -- everything Mira itself owns (id, source, tags,
  // prefix) is set here regardless of what that call returns. Output:
  // {"appName": "...", "title": "...", "tasks": [{"category":
  // "game", "type": "FileTask", "isPrimary": true, "path": "Game.exe"},
  // ...]} -- the primary "game" FileTask's path is the real launch target,
  // no heuristic detection needed the way EpicImporter/ItchImporter use.
  std::string title;
  std::string exe_path;
  const bool known = existing && !existing->exe_path.empty() && existing->install_path == game_dir.string();
  if (!refresh && known) {
    title = existing->name;
  } else if (const Result<std::string> imported = RunGogdl(config_, {"import", game_dir.string()}); imported) {
    const json parsed = core::ParseJsonTail(*imported);
    if (!parsed.is_discarded() && parsed.is_object()) {
      title = parsed.value("title", std::string());
      for (const json& task : parsed.value("tasks", json::array())) {
        if (task.value("category", std::string()) == "game" && task.value("type", std::string()) == "FileTask" &&
            task.value("isPrimary", false)) {
          exe_path = task.value("path", std::string());
          break;
        }
      }
    }
  } else {
    log::Warn("gogdl import at {} failed: {}", game_dir.string(), imported.error().message);
  }

  model::Game game = existing.value_or(model::Game{});
  game.id = game_id;
  game.source = "gog";
  game.source_ref = id;
  game.name = title.empty() ? id : title;
  game.install_path = game_dir.string();
  if (!exe_path.empty()) game.exe_path = exe_path;
  game.platform = model::Platform::Windows;
  game.last_error.clear();
  game.updated_at = model::NowSeconds();
  if (!existing) game.created_at = game.updated_at;

  const runner::RunnerRegistry provisioner(config_);
  library::ProvisionOnImport(game, existing, config_, games_, provisioner);

  auto result = games_.Merge(existing, game);
  if (!result) return std::unexpected(result.error());

  events_.Publish(existing ? "game.updated" : "game.added", model::ToJson(game));
  return game;
}

Result<library::ImportSummary> GogImporter::Import() {
  library::ImportSummary summary;
  if (!config_.GetBool("gog.enabled")) return summary;

  const fs::path root = InstallRoot(config_);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return summary;  // nothing installed yet is not an error

  for (const fs::path& entry : paths::ListDir(root)) {
    if (!fs::is_directory(entry, ec)) continue;
    // No goggame-*.info yet: gogdl is still downloading it, unless it's the
    // legacy <install_root>/<id>/ layout.
    const std::string folder = entry.filename().string();
    const bool legacy_id = !folder.empty() && std::ranges::all_of(folder, [](char c) { return c >= '0' && c <= '9'; });
    const std::optional<std::string> found = GogIdIn(ResolveGameDir(entry));
    if (!found && !legacy_id) continue;
    const std::string id = found.value_or(folder);
    const bool existed = games_.Find("gog-" + id).has_value();

    const Result<model::Game> imported = ImportPath(id, entry, /*refresh=*/false);
    if (!imported) {
      log::Error("failed to import gog game at {}: {}", entry.string(), imported.error().message);
      continue;
    }
    if (existed) {
      ++summary.updated;
    } else {
      ++summary.added;
      summary.added_games.push_back(*imported);
    }
  }
  return summary;
}

}  // namespace mira::gog
