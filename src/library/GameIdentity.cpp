#include "library/GameIdentity.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>

#include "config/Config.h"
#include "config/Resolver.h"
#include "core/Files.h"
#include "core/Strings.h"
#include "emulators/Emulators.h"
#include "gog/GameInfo.h"
#include "library/PeVersion.h"
#include "runner/UmuDatabase.h"

namespace mira::library {
namespace {

namespace fs = std::filesystem;

bool Digits(const std::string& text) {
  return !text.empty() &&
         std::ranges::all_of(text, [](unsigned char c) { return c >= '0' && c <= '9'; });
}

// The store a game was imported from, as umu names it, and its id there.
std::pair<std::string, std::string> ImportedFrom(const model::Game& game) {
  if (game.runner_ref.starts_with("steam:")) return {"steam", game.runner_ref.substr(6)};
  // A launcher's games (Ubisoft, EA, Battle.net) carry umu's name for it.
  if (const auto it = game.runner_config.find("store");
      it != game.runner_config.end() && it->is_string()) {
    return {it->get<std::string>(), game.source_ref.substr(0, game.source_ref.find(','))};
  }
  static const std::map<std::string, std::string, std::less<>> kStores = {{"epic", "egs"},
                                                                          {"gog", "gog"},
                                                                          {"amazon", "amazon"},
                                                                          {"humble", "humble"},
                                                                          {"itch", "itchio"}};
  std::string source = game.source;
  std::string ref = game.source_ref;
  // Heroic's refs are "<store>:<id>".
  if (source == "heroic") {
    const auto colon = ref.find(':');
    if (colon == std::string::npos) return {};
    source = ref.substr(0, colon);
    ref = ref.substr(colon + 1);
  }
  const auto it = kStores.find(source);
  if (it == kStores.end() || ref.empty()) return {};
  return {it->second, ref};
}

// The appid in a steam_appid.txt the game ships beside its exe or in its folder.
std::string SteamAppIdFile(const model::Game& game) {
  if (game.install_path.empty()) return {};
  const fs::path root = game.install_path;
  for (const fs::path& dir : {(root / game.exe_path).parent_path(), root}) {
    if (const auto text = files::ReadFile(dir / "steam_appid.txt")) {
      if (std::string id = strings::Trim(*text); Digits(id)) return id;
    }
  }
  return {};
}

// The name the game's exe gives itself, cleaned; empty when it's a generic one ("Launcher").
// Remembered per exe and its change time, since a resource section can be large.
std::string ExeTitle(const model::Game& game) {
  if (game.install_path.empty() || game.exe_path.empty()) return {};
  const fs::path exe = fs::path(game.install_path) / game.exe_path;
  std::error_code ec;
  const auto modified = fs::last_write_time(exe, ec);
  if (ec) return {};
  static std::mutex mutex;
  static std::map<fs::path, std::pair<fs::file_time_type, std::string>> seen;
  const std::lock_guard lock(mutex);
  if (const auto it = seen.find(exe); it != seen.end() && it->second.first == modified)
    return it->second.second;
  std::string title = strings::CleanGameName(PeProductName(exe));
  if (strings::IsGenericName(title)) title.clear();
  seen[exe] = {modified, title};
  return title;
}

}  // namespace

GameIdentity Identify(const config::Config& config, const model::Game& game) {
  GameIdentity identity;
  // A store's launcher, or an emulator's game, is no Steam or umu game.
  if (game.source == "launcher" || emulators::ForGame(game)) return identity;

  std::tie(identity.store, identity.store_id) = ImportedFrom(game);
  // An offline GOG installer leaves the same file a GOG install does.
  if (identity.store.empty() && !game.install_path.empty()) {
    if (auto gog = gog::ReadGameInfo(game.install_path)) {
      identity.store = "gog";
      identity.store_id = std::move(gog->id);
      identity.title = std::move(gog->name);
    }
  }

  std::optional<runner::UmuRow> row = runner::UmuRowFor(config, identity.store, identity.store_id);
  const bool app = std::ranges::contains(game.tags, std::string("app"));
  if (game.platform == model::Platform::Windows && !app) identity.exe_title = ExeTitle(game);
  // By title only for games: an app's name ("Word") is no game's.
  if (!app) {
    for (const std::string* title :
         {&std::as_const(identity.title), &game.name, &std::as_const(identity.exe_title)}) {
      if (!row && !title->empty()) row = runner::UmuRowByTitle(config, *title);
    }
  }
  if (row && identity.title.empty()) identity.title = row->title;

  const auto steam = [&](std::string appid, const char* from) {
    if (!identity.steam_appid.empty() || !Digits(appid)) return;
    identity.steam_appid = std::move(appid);
    identity.steam_from = from;
  };
  // A Steam game is its own; anything else can be told which it is.
  if (identity.store == "steam") steam(identity.store_id, "steam");
  if (const std::int64_t fixed =
          config::Resolver(config, game.overrides).GetInt("metadata.steam_appid");
      fixed > 0)
    steam(std::to_string(fixed), "override");
  steam(SteamAppIdFile(game), "file");
  if (row) steam(runner::SteamAppId(row->umu_id), "umu");
  if (config.GetBool("metadata.steam_by_name")) steam(game.steam_match, "name");

  // The database's row names the store's fixes; a Steam game chosen by hand wins over it.
  if (row && identity.steam_from != "override") {
    identity.umu_id = row->umu_id;
  } else if (!identity.steam_appid.empty()) {
    identity.umu_id = "umu-" + identity.steam_appid;
  }
  return identity;
}

nlohmann::json ToJson(const GameIdentity& identity) {
  nlohmann::json out = nlohmann::json::object();
  const auto put = [&](const char* key, const std::string& value) {
    if (!value.empty()) out[key] = value;
  };
  put("store", identity.store);
  put("store_id", identity.store_id);
  put("steam_appid", identity.steam_appid);
  put("steam_from", identity.steam_from);
  put("umu_id", identity.umu_id);
  put("title", identity.title);
  put("exe_title", identity.exe_title);
  return out;
}

}  // namespace mira::library
