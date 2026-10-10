#include "lutris/LutrisImporter.h"

#include <fkYAML.hpp>
#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/Detector.h"
#include "library/Relocate.h"
#include "runner/Exec.h"

namespace mira::lutris {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Lutris's own settings.py: CONFIG_DIR = get_user_config_dir()/lutris, but
// falls back to DATA_DIR when that doesn't exist, which is where the
// per-game YAML actually lives on a system that never had ~/.config/lutris.
struct LutrisRow {
  int id = 0;
  std::string name;
  std::string slug;
  std::string runner;
  std::string configpath;
  std::vector<std::string> categories;  // Mira tags, already mapped
  std::int64_t lastplayed = 0;  // unix seconds; 0 if never
  double playtime_hours = 0;
};

// Runs one query and returns its rows. An empty result set prints nothing at all, not "[]".
Result<json> Query(const std::string& sqlite3_bin, const fs::path& pga_db, const std::string& sql) {
  Command command;
  command.argv = {sqlite3_bin, "-json", pga_db.string(), sql};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) {
    return Err("lutris_db_read_failed",
               !result ? result.error().message
                       : std::format("sqlite3 exited {}: {}", result->exit_code, result->output));
  }
  if (result->output.find_first_not_of(" \t\r\n") == std::string::npos) return json::array();
  json parsed = json::parse(result->output, nullptr, false);
  if (!parsed.is_array()) return Err("lutris_db_parse_failed", "unexpected output from sqlite3 -json");
  return parsed;
}

// Lutris's "hidden" is a category named ".hidden", and "favorite" is
// "favorites", both discovered from a real pga.db, not Lutris's docs (see
// lutris/game.py's is_hidden/mark_as_hidden). Everything else maps straight
// across as a same-named tag: a category is meant to organize games, which
// is exactly what a tag already does in Mira.
std::string TagForCategory(const std::string& category) {
  if (category == ".hidden") return "hidden";
  if (category == "favorites") return "favorite";
  return category;
}

// The games with their categories and play history in one query. Older
// Lutris versions lack the categories tables or the playtime column, and
// each missing piece degrades to "none" rather than failing the whole import.
Result<std::vector<LutrisRow>> ReadCatalog(const std::string& sqlite3_bin, const fs::path& pga_db) {
  constexpr char kPlain[] = "SELECT id, name, slug, runner, configpath FROM games;";
  constexpr char kWithCategories[] =
      "SELECT games.id AS id, games.name AS name, games.slug AS slug, games.runner AS runner, "
      "games.configpath AS configpath, group_concat(categories.name, char(31)) AS categories FROM games "
      "LEFT JOIN games_categories ON games_categories.game_id = games.id "
      "LEFT JOIN categories ON categories.id = games_categories.category_id GROUP BY games.id;";
  constexpr char kWithPlay[] =
      "SELECT games.id AS id, games.name AS name, games.slug AS slug, games.runner AS runner, "
      "games.configpath AS configpath, games.lastplayed AS lastplayed, games.playtime AS playtime, "
      "group_concat(categories.name, char(31)) AS categories FROM games "
      "LEFT JOIN games_categories ON games_categories.game_id = games.id "
      "LEFT JOIN categories ON categories.id = games_categories.category_id GROUP BY games.id;";
  Result<json> parsed = Query(sqlite3_bin, pga_db, kWithPlay);
  if (!parsed) parsed = Query(sqlite3_bin, pga_db, kWithCategories);
  if (!parsed) parsed = Query(sqlite3_bin, pga_db, kPlain);
  if (!parsed) return std::unexpected(parsed.error());

  std::vector<LutrisRow> rows;
  for (const json& row : *parsed) {
    LutrisRow entry{
        .id = static_cast<int>(core::JsonInt(row, "id")),
        .name = core::JsonString(row, "name"),
        .slug = core::JsonString(row, "slug"),
        .runner = core::JsonString(row, "runner"),
        .configpath = core::JsonString(row, "configpath"),
        .categories = {},
        .lastplayed = core::JsonInt(row, "lastplayed"),
        .playtime_hours = row.contains("playtime") && row["playtime"].is_number() ? row["playtime"].get<double>() : 0,
    };
    for (const std::string& category : strings::Split(core::JsonString(row, "categories"), '\x1f')) {
      if (!category.empty()) entry.categories.push_back(TagForCategory(category));
    }
    rows.push_back(std::move(entry));
  }
  return rows;
}

// Union, not replace: a re-import must keep tags the user added by hand,
// same contract as every other field Lutris doesn't own (see Import()'s
// comment below).
std::vector<std::string> MergeTags(std::vector<std::string> existing, const std::vector<std::string>& lutris_tags) {
  for (const std::string& tag : lutris_tags) {
    if (std::ranges::find(existing, tag) == existing.end()) existing.push_back(tag);
  }
  return existing;
}

std::string NodeToString(const fkyaml::node& node) {
  if (node.is_string()) return node.get_value<std::string>();
  if (node.is_boolean()) return node.get_value<bool>() ? "true" : "false";
  if (node.is_integer()) return std::to_string(node.get_value<std::int64_t>());
  if (node.is_float_number()) return std::to_string(node.get_value<double>());
  return "";
}

struct LutrisGameConfig {
  std::string exe;
  std::string prefix;  // empty for a native-Linux ("linux" runner) row -- no prefix concept at all
  std::string args;
  std::map<std::string, std::string> env;
};

// `requires_prefix` distinguishes the two runner kinds this importer
// handles: a "wine" row needs both exe and prefix (see the no-prefix skip
// note below); a "linux" row (a native .sh/AppImage game) has no prefix
// concept whatsoever, so only exe is required.
std::optional<LutrisGameConfig> ReadGameConfig(const fs::path& yaml_path, bool requires_prefix) {
  std::ifstream in(yaml_path);
  if (!in) return std::nullopt;
  std::stringstream buffer;
  buffer << in.rdbuf();

  LutrisGameConfig cfg;
  try {
    const fkyaml::node root = fkyaml::node::deserialize(buffer.str());
    if (!root.is_mapping() || !root.contains("game")) return std::nullopt;
    const fkyaml::node& game = root["game"];
    if (game.contains("exe") && game["exe"].is_string()) cfg.exe = game["exe"].get_value<std::string>();
    if (game.contains("prefix") && game["prefix"].is_string()) cfg.prefix = game["prefix"].get_value<std::string>();
    if (game.contains("args") && game["args"].is_string()) cfg.args = game["args"].get_value<std::string>();

    if (root.contains("system") && root["system"].is_mapping() && root["system"].contains("env") &&
        root["system"]["env"].is_mapping()) {
      for (const auto& [key, value] : root["system"]["env"].map_items()) {
        cfg.env[key.get_value<std::string>()] = NodeToString(value);
      }
    }
  } catch (const std::exception& e) {
    log::Warn("failed to parse Lutris config {}: {}", yaml_path.string(), e.what());
    return std::nullopt;
  }

  // No inference beyond what the yaml itself declares: a wine row with no
  // prefix recorded is skipped rather than guessed at (Lutris's own
  // fallback for this case is a filesystem walk-up heuristic, not something
  // read from the yaml tree).
  if (cfg.exe.empty() || (requires_prefix && cfg.prefix.empty())) return std::nullopt;
  return cfg;
}

}  // namespace

// Lutris's own settings.py: CONFIG_DIR = get_user_config_dir()/lutris, but
// falls back to DATA_DIR when that doesn't exist, which is where the
// per-game YAML actually lives on a system that never had ~/.config/lutris.
std::optional<fs::path> FindLutrisDataDir(const config::Config& config) {
  std::vector<fs::path> candidates;
  if (const fs::path configured = config.GetPath("lutris.data_dir"); !configured.empty()) {
    candidates.push_back(configured);
  }
  candidates.push_back(paths::EnvOr("XDG_DATA_HOME", paths::Home() / ".local" / "share") / "lutris");

  std::error_code ec;
  for (const fs::path& candidate : candidates) {
    if (fs::exists(candidate / "pga.db", ec)) return candidate;
  }
  return std::nullopt;
}

LutrisImporter::LutrisImporter(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<LutrisImportSummary> LutrisImporter::Import() {
  LutrisImportSummary summary;
  if (!games_.SourceEnabled("lutris")) return summary;

  const auto data_dir = FindLutrisDataDir(config_);
  if (!data_dir) {
    return Err("lutris_not_found", "Mira couldn't find a Lutris installation",
               "If Lutris is installed somewhere unusual, set its data folder.", Fix::Setting("lutris.data_dir"));
  }

  const auto sqlite3 = runner::FindOnPath("sqlite3");
  if (!sqlite3) {
    return Err("sqlite3_missing",
               "sqlite3 isn't installed. Install it from your distro's package manager to read "
               "Lutris's game database");
  }
  const fs::path pga_db = *data_dir / "pga.db";

  const auto rows = ReadCatalog(*sqlite3, pga_db);
  if (!rows) return std::unexpected(rows.error());

  for (const LutrisRow& row : *rows) {
    // "wine" is a Windows game; "linux" is Lutris's own native-Linux runner
    // (a .sh script or an AppImage, pointed at directly, no prefix at all).
    // Anything else is skipped (a "steam" row is
    // already covered by SteamScanner, a "flatpak" row's app already has its
    // own real .desktop entry, covered by desktop::DesktopEntryScanner),
    // note it, don't guess at it.
    const bool is_native = row.runner == "linux";
    if (row.runner != "wine" && !is_native) {
      ++summary.other_runner;
      continue;
    }

    const fs::path yaml_path = *data_dir / "games" / (row.configpath + ".yml");
    const auto cfg = ReadGameConfig(yaml_path, /*requires_prefix=*/!is_native);
    if (!cfg) {
      ++summary.incomplete;
      continue;
    }

    // Nothing here is inferred from how exe and prefix relate to each other
    // on disk: an exe given relative is relative to prefix because that's
    // what Lutris's own config format declares (see Epic's real config:
    // exe under drive_c, prefix $GAMEDIR), not because Mira went looking. A
    // "linux" row has no prefix at all, so its exe is always given as an
    // absolute path (Lutris's own linux.py: a plain file picker, nothing to
    // resolve relative to), so a relative one here has nothing to resolve
    // against and is skipped rather than guessed at.
    const fs::path prefix = fs::path(cfg->prefix);
    const fs::path exe_raw = fs::path(cfg->exe);
    if (is_native && !exe_raw.is_absolute()) {
      ++summary.incomplete;
      continue;
    }
    const fs::path exe_abs = exe_raw.is_absolute() ? exe_raw : prefix / exe_raw;

    const std::string install_path = exe_abs.parent_path().string();
    const std::string exe_path = exe_abs.filename().string();
    const std::string data_dir_path = is_native ? std::string() : prefix.string();

    // Not a layout guess: a safety floor for one specific real shape,
    // wine-only (a native row has no prefix to share in the first place):
    // an exe referenced with no subdirectory at all under drive_c (a
    // launcher script dropped straight at the C: drive root, e.g. a second
    // game riding along in another game's prefix). That makes install_path
    // the whole C: drive, shared by every other game in that prefix,
    // Mira's DELETE /v1/games removes install_path's contents, so handing
    // out a scope that broad would let deleting this game take the others
    // with it. A combined install+prefix layout (install_path == prefix
    // itself, e.g. Batman) is fine and left alone: that prefix belongs to
    // this game alone.
    if (!is_native && install_path == (prefix / "drive_c").string()) {
      log::Warn("skipping lutris game {}: install path {} is drive_c's own root, not something game-specific",
               row.name, install_path);
      ++summary.incomplete;
      continue;
    }

    // Moved into Mira's folders, so Mira's own game now (see library::Relocate).
    const std::string claimed = std::string(library::kClaimedLutrisPrefix) + row.slug;
    if (!row.slug.empty() && std::ranges::any_of(games_.All(), [&](const model::Game& known) {
          return known.source_ref == claimed;
        })) {
      continue;
    }

    // By slug too: a game moved into Mira's folders no longer sits at Lutris's path.
    auto existing = games_.FindByInstallPath(install_path);
    if (!existing && !row.slug.empty()) {
      for (const model::Game& known : games_.All()) {
        if (known.source == "lutris" && known.source_ref == row.slug) existing = known;
      }
    }

    // Moved by an older Mira, which left it a Lutris game: Lutris points at
    // folders that are gone, so it's Mira's own now, as a move makes it today.
    std::error_code ec;
    const bool install_moved = existing && existing->install_path != install_path && !fs::exists(exe_abs, ec);
    const bool prefix_moved =
        existing && !data_dir_path.empty() && existing->data_dir != data_dir_path && !fs::exists(prefix, ec);
    if (install_moved || prefix_moved) {
      if (auto taken = games_.Update(existing->id, [&](model::Game& g) {
            g.source = "manual";
            g.source_ref = claimed;
          })) {
        events_.Publish("game.updated", model::ToJson(*taken));
      }
      continue;
    }

    // Preserve anything the user already configured across a re-import,
    // only the fields Lutris itself owns get overwritten, same contract as
    // SteamScanner::Scan.
    model::Game game = existing.value_or(model::Game{});
    game.id = existing ? game.id : games_.NextId(row.slug.empty() ? row.name : row.slug);
    game.source = "lutris";
    game.source_ref = row.slug;  // joins Lutris's own cached banner/cover/icon files by slug
    game.name = row.name;
    // Lutris sets where the game is when it arrives; after that the
    // executable and folders are Mira's to change (a pick in the game's
    // settings), and Lutris's only come back if Mira's are gone.
    const bool own_program = existing && fs::exists(fs::path(existing->install_path) / existing->exe_path, ec);
    if (!own_program) {
      game.install_path = install_path;
      game.exe_path = exe_path;
    }
    game.args = cfg->args;
    if (!existing || existing->data_dir.empty() || !fs::exists(existing->data_dir, ec)) game.data_dir = data_dir_path;
    // Lutris names one executable; the folder's others are offered to pick from,
    // unless the folder holds other games too.
    if (game.candidates.empty() && library::SharedFolder(config_, game, games_.All()).empty()) {
      game.candidates = library::Detector(library::SettingsFromConfig(config_)).Detect(game.install_path).candidates;
      for (model::Candidate& candidate : game.candidates) candidate.chosen = candidate.rel_path == game.exe_path;
      if (std::ranges::none_of(game.candidates, &model::Candidate::chosen)) {
        game.candidates.insert(game.candidates.begin(),
                               {.rel_path = game.exe_path,
                                .kind = is_native ? model::Platform::Native : model::Platform::Windows,
                                .score = 1.0, .chosen = true,
                                .is_installer = false});
      }
    }
    game.platform = is_native ? model::Platform::Native : model::Platform::Windows;
    game.env = cfg->env;
    // Lutris's record, until Mira has a session of its own: then neither can
    // say which is right, so Mira's own stays.
    if (game.last_session_at == 0) {
      if (row.playtime_hours > 0) game.play_seconds = std::llround(row.playtime_hours * 3600);
      if (row.lastplayed > 0) game.last_played_at = row.lastplayed;
    }
    game.tags = MergeTags(game.tags, row.categories);
    // Lutris's own wine.version is often a generic alias ("ge-proton"), not
    // an exact installed build name Mira can resolve, so leave runner_ref
    // alone (empty for a new game) and let default_runner.windows pick one.
    // A native row needs no runner_ref at all (NativeRunner has no builds).
    game.status = model::GameStatus::Ready;  // Lutris already installed and configured it
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    auto result = games_.Merge(existing, game);
    if (!result) {
      log::Error("failed to save lutris game {}: {}", game.id, result.error().message);
      continue;
    }
    library::RecordImported(summary, events_, game, existing.has_value());
  }

  return summary;
}

}  // namespace mira::lutris
