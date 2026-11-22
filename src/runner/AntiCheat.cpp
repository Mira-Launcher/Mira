#include "runner/AntiCheat.h"

#include <chrono>
#include <filesystem>
#include <json.hpp>
#include <map>
#include <mutex>

#include "config/Config.h"
#include "core/Files.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/GameIdentity.h"
#include "runner/Curl.h"

namespace mira::runner {
namespace {

namespace fs = std::filesystem;

struct List {
  fs::path file;
  fs::file_time_type loaded_at{};
  std::map<std::string, AntiCheat> by_appid;
  std::map<std::string, AntiCheat> by_title;  // Slugify(name)
};

fs::path ListFile() {
  return paths::EnvOr("XDG_CACHE_HOME", paths::Home() / ".cache") / "mira" / "anticheat.json";
}

// The list, reread when the file on disk changes. Callers hold the mutex.
const List& Load() {
  static List list;
  std::error_code ec;
  const fs::path file = ListFile();
  const auto modified = fs::last_write_time(file, ec);
  if (ec) list = List{};
  if (ec || (file == list.file && modified == list.loaded_at)) return list;
  list = List{};
  list.file = file;
  list.loaded_at = modified;
  const auto text = files::ReadFile(file);
  const nlohmann::json parsed =
      text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json();
  if (!parsed.is_array()) return list;
  for (const nlohmann::json& entry : parsed) {
    if (!entry.is_object() || !entry.value("status", nlohmann::json()).is_string()) continue;
    AntiCheat info{.status = entry["status"], .anticheats = {}, .url = {}};
    if (const auto it = entry.find("anticheats"); it != entry.end() && it->is_array()) {
      for (const auto& name : *it)
        if (name.is_string()) info.anticheats.push_back(name);
    }
    if (const auto it = entry.find("reference"); it != entry.end() && it->is_string())
      info.url = *it;
    if (const auto ids = entry.find("storeIds"); ids != entry.end() && ids->is_object()) {
      if (const auto steam = ids->find("steam"); steam != ids->end() && steam->is_string())
        list.by_appid.emplace(*steam, info);
    }
    if (const auto name = entry.find("name"); name != entry.end() && name->is_string())
      list.by_title.emplace(strings::Slugify(name->get<std::string>()), std::move(info));
  }
  return list;
}

std::mutex& ListMutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace

void RefreshAntiCheatDatabase(const config::Config& config) {
  if (!config.GetBool("anticheat.check")) return;
  if (auto got = RefreshDownload(config.GetString("anticheat.database_url"), ListFile(),
                                 std::chrono::hours(24 * 7), 32 << 20);
      !got)
    log::Warn("could not download the anti-cheat list: {}", got.error().message);
}

std::optional<AntiCheat> FindAntiCheat(const config::Config& config, const model::Game& game) {
  if (!config.GetBool("anticheat.check")) return std::nullopt;
  if (!game.runner_ref.starts_with("steam:") && game.platform != model::Platform::Windows)
    return std::nullopt;
  const library::GameIdentity identity = library::Identify(config, game);
  const std::lock_guard lock(ListMutex());
  const List& list = Load();
  if (!identity.steam_appid.empty()) {
    if (const auto it = list.by_appid.find(identity.steam_appid); it != list.by_appid.end())
      return it->second;
  }
  const std::string& title = identity.title.empty() ? game.name : identity.title;
  if (title.empty()) return std::nullopt;
  const auto it = list.by_title.find(strings::Slugify(title));
  return it == list.by_title.end() ? std::nullopt : std::optional(it->second);
}

}  // namespace mira::runner
