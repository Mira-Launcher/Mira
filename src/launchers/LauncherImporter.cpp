#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

#include <json.hpp>

#include "core/Log.h"
#include "core/StoreErrors.h"
#include "core/Strings.h"
#include "launchers/Launchers.h"
#include "launchers/Office.h"
#include "library/Detector.h"
#include "runner/Exec.h"

namespace mira::launchers {
namespace {
namespace fs = std::filesystem;

struct Found {
  std::string ref;   // what the launcher's own launch command takes
  std::string name;
  fs::path dir;
  std::string exe = {};  // known up front (Microsoft 365); else detected
};

// Battle.net keeps its install list in a protobuf (product.db); default
// install folders are enough to find the games people actually play.
struct KnownGame {
  std::string_view code;
  std::string_view name;
  std::string_view folder;
};
constexpr KnownGame kBattleNetGames[] = {
    {"WTCG", "Hearthstone", "Hearthstone"},
    {"Fen", "Diablo IV", "Diablo IV"},
    {"OSI", "Diablo II: Resurrected", "Diablo II Resurrected"},
    {"D3", "Diablo III", "Diablo III"},
    {"ANBS", "Diablo Immortal", "Diablo Immortal"},
    {"Pro", "Overwatch 2", "Overwatch"},
    {"WoW", "World of Warcraft", "World of Warcraft"},
    {"Hero", "Heroes of the Storm", "Heroes of the Storm"},
    {"S1", "StarCraft", "StarCraft"},
    {"S2", "StarCraft II", "StarCraft II"},
    {"W3", "Warcraft III", "Warcraft III"},
};

std::vector<Found> FindBattleNet(const fs::path& prefix) {
  std::vector<Found> found;
  std::error_code ec;
  for (const KnownGame& game : kBattleNetGames) {
    for (const char* program_files : {"Program Files (x86)", "Program Files"}) {
      const fs::path dir = prefix / "drive_c" / program_files / game.folder;
      if (!fs::is_directory(dir, ec)) continue;
      found.push_back({std::string(game.code), std::string(game.name), dir});
      break;
    }
  }
  return found;
}

std::vector<Found> FindUbisoft(const fs::path& prefix) {
  std::vector<Found> found;
  std::error_code ec;
  for (const char* key : {"Software\\Wow6432Node\\Ubisoft\\Launcher\\Installs", "Software\\Ubisoft\\Launcher\\Installs"}) {
    for (const auto& [id, values] : ReadRegSubkeys(prefix / "system.reg", key)) {
      const auto install_dir = values.find("InstallDir");
      if (install_dir == values.end()) continue;
      const fs::path dir = HostPath(prefix, install_dir->second);
      if (dir.empty() || !fs::is_directory(dir, ec)) continue;
      found.push_back({id, dir.filename().string(), dir});
    }
    if (!found.empty()) break;
  }
  return found;
}

// Text of every <tag>...</tag> in `xml`; enough for installerdata.xml.
std::vector<std::string> TagValues(const std::string& xml, std::string_view tag) {
  std::vector<std::string> values;
  const std::string open = std::format("<{}", tag);
  const std::string close = std::format("</{}>", tag);
  for (std::size_t at = xml.find(open); at != std::string::npos; at = xml.find(open, at + 1)) {
    // Not <contentIDs>, the list <contentID> sits in.
    const char next = at + open.size() < xml.size() ? xml[at + open.size()] : '\0';
    if (next != '>' && next != ' ') continue;
    const std::size_t start = xml.find('>', at);
    const std::size_t end = xml.find(close, at);
    if (start == std::string::npos || end == std::string::npos || start > end) break;
    values.push_back(xml.substr(start + 1, end - start - 1));
  }
  return values;
}

std::vector<Found> FindEa(const fs::path& prefix) {
  std::vector<Found> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(prefix / "drive_c/Program Files/EA Games", ec)) {
    std::ifstream in(entry.path() / "__Installer/installerdata.xml");
    if (!in) continue;
    std::stringstream xml;
    xml << in.rdbuf();
    const std::vector<std::string> ids = TagValues(xml.str(), "contentID");
    if (ids.empty()) continue;
    const std::vector<std::string> titles = TagValues(xml.str(), "gameTitle");
    std::string joined;
    for (const std::string& id : ids) joined += (joined.empty() ? "" : ",") + id;
    found.push_back({joined, titles.empty() ? entry.path().filename().string() : titles.front(), entry.path()});
  }
  return found;
}

std::vector<Found> FindOffice(const fs::path& prefix) {
  std::vector<Found> found;
  const fs::path dir = prefix / "drive_c" / office::kProgramDir;
  std::error_code ec;
  for (const office::App& app : office::Apps()) {
    if (!fs::is_regular_file(dir / app.exe, ec)) continue;
    found.push_back({std::string(app.ref), std::string(app.name), dir, std::string(app.exe)});
  }
  return found;
}

// umu's id for a store game, so protonfixes find it. Best effort.
std::string LookupUmuId(const std::string& store, const std::string& ref) {
  const std::string codename = ref.substr(0, ref.find(','));
  Command command;
  command.argv = {"curl", "-sSf", "--max-time", "5", "--get", "--data-urlencode", "codename=" + codename,
                  "--data-urlencode", "store=" + store, "https://umu.openwinecomponents.org/umu_api.php"};
  const auto result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) return "";
  const nlohmann::json parsed = nlohmann::json::parse(result->output, nullptr, false);
  if (!parsed.is_array() || parsed.empty() || !parsed[0].is_object()) return "";
  return parsed[0].value("umu_id", std::string());
}

}  // namespace

Result<ImportSummary> Import(config::Config& config, store::GameStore& games, api::EventBus& events,
                             const Launcher& launcher) {
  const auto host = games.Find(GameId(launcher));
  if (!host || host->status != model::GameStatus::Ready) {
    return LauncherNotInstalled(launcher.id, launcher.name);
  }
  const fs::path prefix = host->data_dir;
  std::vector<Found> found;
  if (launcher.id == "battlenet") found = FindBattleNet(prefix);
  if (launcher.id == "ubisoft") found = FindUbisoft(prefix);
  if (launcher.id == "ea") found = FindEa(prefix);
  if (launcher.id == "office") found = FindOffice(prefix);

  ImportSummary summary;
  const auto batch = games.BatchSaves();
  const library::Detector detector(library::SettingsFromConfig(config));
  for (const Found& item : found) {
    const std::string slug = strings::Slugify(launcher.id == "ea" ? item.dir.filename().string() : item.ref);
    const std::string id = std::format("{}-{}", launcher.id, slug);
    const auto existing = games.Find(id);

    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.source = launcher.id;
    game.source_ref = item.ref;
    if (game.name.empty()) game.name = item.name;
    game.install_path = item.dir.string();
    game.platform = model::Platform::Windows;
    game.data_dir = host->data_dir;
    game.runner_ref = host->runner_ref;
    for (const auto& [key, value] : launcher.env) game.env.try_emplace(key, value);
    if (!launcher.umu_store.empty()) {
      game.runner_config["store"] = launcher.umu_store;
      if (!existing && config.GetBool("launchers.umu_lookup")) {
        if (const std::string umu_id = LookupUmuId(launcher.umu_store, item.ref); !umu_id.empty()) {
          game.runner_config["gameid"] = umu_id;
        }
      }
    }
    if (!item.exe.empty()) game.exe_path = item.exe;
    // Launched through the launcher; the exe is only for art and menus.
    if (game.exe_path.empty()) {
      const library::Detector::Result detected = detector.Detect(item.dir);
      game.candidates = detected.candidates;
      const auto exe = std::ranges::find(detected.candidates, false, &model::Candidate::is_installer);
      if (exe != detected.candidates.end()) game.exe_path = exe->rel_path;
    }
    if (std::ranges::find(game.tags, launcher.id) == game.tags.end()) game.tags.push_back(launcher.id);
    if (launcher.id == "office" && std::ranges::find(game.tags, "app") == game.tags.end()) game.tags.push_back("app");
    game.status = model::GameStatus::Ready;
    game.last_error.clear();
    if (existing && model::ToJson(game) == model::ToJson(*existing)) continue;  // nothing new
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    if (auto saved = games.Merge(existing, game); !saved) {
      log::Error("failed to import {} game {}: {}", launcher.name, id, saved.error().message);
      continue;
    }
    events.Publish(existing ? "game.updated" : "game.added", model::ToJson(game));
    if (existing) {
      ++summary.updated;
    } else {
      ++summary.added;
      summary.added_games.push_back(game);
    }
  }

  // Uninstalled through the launcher: handled like a scanned game whose
  // folder disappeared.
  for (const model::Game& game : games.All()) {
    std::error_code ec;
    if (game.source != launcher.id || game.status == model::GameStatus::Missing) continue;
    if (fs::is_directory(game.install_path, ec)) continue;
    if (config.GetBool("library.remove_missing")) {
      if (games.Remove(game.id)) events.Publish("game.removed", {{"id", game.id}});
      continue;
    }
    if (auto missing = games.Update(game.id, [](model::Game& g) { g.status = model::GameStatus::Missing; })) {
      events.Publish("game.updated", model::ToJson(*missing));
    }
  }
  return summary;
}

ImportSummary ImportAll(config::Config& config, store::GameStore& games, api::EventBus& events) {
  ImportSummary total;
  for (const Launcher& launcher : All()) {
    if (!Installed(games, launcher) || !config.GetBool(launcher.id + ".enabled")) continue;
    const auto summary = Import(config, games, events, launcher);
    if (!summary) continue;
    total.added += summary->added;
    total.updated += summary->updated;
    std::ranges::move(summary->added_games, std::back_inserter(total.added_games));
  }
  return total;
}

}  // namespace mira::launchers
