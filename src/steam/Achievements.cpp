#include "steam/Achievements.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>

#include "core/Files.h"
#include "core/Json.h"
#include "runner/Curl.h"
#include "runner/Exec.h"
#include "steam/BinaryVdf.h"
#include "steam/SteamDetector.h"

namespace mira::steam {
namespace {
using achievements::Achievement;
using achievements::GameAchievements;
using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uint64_t kSteamId64Base = 76561197960265728ULL;

std::string ApiBase() {
  return achievements::Endpoint("STEAM_API", "https://api.steampowered.com");
}

// The first object in a stats file: {"<appid>": {...}} for a schema, {"cache": {...}} for progress.
std::optional<Node> ReadStatsFile(const fs::path& file) {
  const auto data = files::ReadFile(file);
  if (!data) return std::nullopt;
  Reader reader{*data};
  Node top;
  if (!reader.Object(top) || top.children.size() != 1 || top.children[0].type != kObject)
    return std::nullopt;
  return std::move(top.children[0]);
}

// A schema's text in `language`, else English, else whichever language it has.
std::string Localized(Node* texts, const std::string& language) {
  if (texts == nullptr) return {};
  if (texts->type == kString) return texts->bytes;
  for (const char* key : {language.c_str(), "english"}) {
    if (const Node* text = texts->Child(key); text != nullptr && text->type == kString)
      return text->bytes;
  }
  for (const Node& text : texts->children) {
    if (text.type == kString && text.name != "token") return text.bytes;
  }
  return {};
}

bool Flag(const Node* node) {
  if (node == nullptr) return false;
  if (node->type == kString) return node->bytes == "1" || node->bytes == "true";
  return node->Int32() != 0;
}

int Number(std::string_view text) {
  int value = -1;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

// The account whose progress to read: steam.steamid64, else the one Steam signed in last.
std::string AccountId(const config::Config& config, const fs::path& root) {
  std::string id64 = config.GetString("steam.steamid64");
  if (id64.empty()) {
    const std::vector<SteamAccount> accounts = Accounts(root);
    if (!accounts.empty()) id64 = accounts.front().steamid64;
  }
  std::uint64_t value = 0;
  if (std::from_chars(id64.data(), id64.data() + id64.size(), value).ec != std::errc() ||
      value < kSteamId64Base)
    return {};
  return std::to_string(value - kSteamId64Base);
}

GameAchievements FromLocalFiles(Node& schema, std::optional<Node>& progress,
                                const std::string& appid, const std::string& language) {
  const std::string icons =
      std::format("{}/{}/",
                  achievements::Endpoint(
                      "STEAM_ICONS", "https://shared.steamstatic.com/community_assets/images/apps"),
                  appid);
  GameAchievements set{.source = "steam", .fetched_at = 0, .items = {}};
  Node* stats = schema.Child("stats");
  if (stats == nullptr) return set;
  for (Node& stat : stats->children) {
    Node* bits = stat.Child("bits");
    if (bits == nullptr) continue;
    // The stat's unlocked bits and each one's time, in the account's progress file.
    std::uint32_t data = 0;
    Node* times = nullptr;
    if (progress) {
      if (Node* saved = progress->Child(stat.name); saved != nullptr) {
        if (const Node* bitfield = saved->Child("data")) data = bitfield->Int32();
        times = saved->Child("AchievementTimes");
      }
    }
    for (Node& bit : bits->children) {
      const int index = Number(bit.name);
      const Node* key = bit.Child("name");
      if (index < 0 || index > 31 || key == nullptr) continue;
      Node* display = bit.Child("display");
      Achievement item;
      item.key = key->bytes;
      if (display != nullptr) {
        item.name = Localized(display->Child("name"), language);
        item.description = Localized(display->Child("desc"), language);
        item.hidden = Flag(display->Child("hidden"));
        if (const Node* icon = display->Child("icon"); icon != nullptr && !icon->bytes.empty())
          item.icon_unlocked = icons + icon->bytes;
        if (const Node* icon = display->Child("icon_gray"); icon != nullptr && !icon->bytes.empty())
          item.icon_locked = icons + icon->bytes;
      }
      item.unlocked = (data >> index) & 1U;
      if (item.unlocked && times != nullptr) {
        if (const Node* when = times->Child(bit.name)) {
          item.unlocked_at = static_cast<std::uint32_t>(when->Int32());
        }
      }
      set.items.push_back(std::move(item));
    }
  }
  return set;
}

// The Web API's answer to `method` for this game, its key kept off curl's command line.
Result<json> WebApi(const std::string& key, const std::string& method,
                    const std::vector<std::string>& query) {
  std::vector<std::string> args = {"-sSL", "-f", "--max-time", "15", "--get"};
  for (const std::string& pair : query) {
    args.push_back("--data-urlencode");
    args.push_back(pair);
  }
  args.push_back(std::format("{}/ISteamUserStats/{}", ApiBase(), method));
  const auto result =
      runner::RunCurlWithSecrets(args, runner::CurlConfigLine("data-urlencode", "key=" + key));
  if (!result) return std::unexpected(result.error());
  const json parsed = json::parse(result->output, nullptr, false);
  if (result->exit_code != 0 || parsed.is_discarded()) {
    return Err("steam_web_api_error", "Steam's Web API didn't return achievements",
               kConnectionHint);
  }
  return parsed;
}

Result<GameAchievements> FromWebApi(const config::Config& config, const std::string& appid,
                                    const std::string& language) {
  const std::string key = config.GetString("steam.web_api_key");
  const std::string steamid = config.GetString("steam.steamid64");
  const auto schema = WebApi(key, "GetSchemaForGame/v2", {"appid=" + appid, "l=" + language});
  if (!schema) return std::unexpected(schema.error());
  const auto player = WebApi(key, "GetPlayerAchievements/v1",
                             {"appid=" + appid, "steamid=" + steamid, "l=" + language});
  if (!player) return std::unexpected(player.error());
  std::map<std::string, std::pair<bool, std::int64_t>> progress;
  const json unlocked = player->value("playerstats", json::object()).value("achievements", json());
  if (unlocked.is_array()) {
    for (const json& entry : unlocked) {
      progress[core::JsonString(entry, "apiname")] = {core::JsonInt(entry, "achieved") != 0,
                                                      core::JsonInt(entry, "unlocktime")};
    }
  }
  GameAchievements set{.source = "steam", .fetched_at = 0, .items = {}};
  const json defined = schema->value("game", json::object())
                           .value("availableGameStats", json::object())
                           .value("achievements", json());
  if (!defined.is_array()) return set;
  for (const json& entry : defined) {
    Achievement item;
    item.key = core::JsonString(entry, "name");
    if (item.key.empty()) continue;
    item.name = core::JsonString(entry, "displayName");
    item.description = core::JsonString(entry, "description");
    item.hidden = core::JsonInt(entry, "hidden") != 0;
    item.icon_unlocked = core::JsonString(entry, "icon");
    item.icon_locked = core::JsonString(entry, "icongray");
    if (const auto found = progress.find(item.key); found != progress.end()) {
      item.unlocked = found->second.first;
      if (item.unlocked) item.unlocked_at = found->second.second;
    }
    set.items.push_back(std::move(item));
  }
  return set;
}

// Each achievement's share of players, from the keyless global percentages; unknown on failure.
void AddRarity(GameAchievements& set, const std::string& appid) {
  if (set.items.empty()) return;
  const auto answer = runner::CurlJson(
      std::format("{}/ISteamUserStats/GetGlobalAchievementPercentagesForApp/v2?gameid={}",
                  ApiBase(), appid),
      15);
  if (!answer) return;
  const json list =
      answer->value("achievementpercentages", json::object()).value("achievements", json());
  if (!list.is_array()) return;
  std::map<std::string, double> percent;
  for (const json& entry : list) {
    const auto value = entry.is_object() ? entry.find("percent") : entry.end();
    if (!entry.is_object() || value == entry.end()) continue;
    // A string in Steam's answer, a number in older ones.
    double share = -1;
    if (value->is_number()) {
      share = value->get<double>();
    } else if (value->is_string()) {
      const std::string text = value->get<std::string>();
      if (std::from_chars(text.data(), text.data() + text.size(), share).ec != std::errc())
        continue;
    }
    percent[core::JsonString(entry, "name")] = share;
  }
  for (Achievement& item : set.items) {
    if (const auto found = percent.find(item.key); found != percent.end())
      item.rarity = found->second;
  }
}

}  // namespace

Result<GameAchievements> FetchAchievements(const config::Config& config, const std::string& appid,
                                           const achievements::Locale& locale) {
  const std::string language = achievements::SteamLanguage(locale);
  Result<GameAchievements> set;
  const auto root = FindSteamRoot(config);
  const fs::path stats = root ? *root / "appcache" / "stats" : fs::path();
  std::optional<Node> schema =
      root ? ReadStatsFile(stats / std::format("UserGameStatsSchema_{}.bin", appid)) : std::nullopt;
  if (schema) {
    const std::string account = AccountId(config, *root);
    std::optional<Node> progress =
        account.empty()
            ? std::nullopt
            : ReadStatsFile(stats / std::format("UserGameStats_{}_{}.bin", account, appid));
    set = FromLocalFiles(*schema, progress, appid, language);
  } else if (!config.GetString("steam.web_api_key").empty() &&
             !config.GetString("steam.steamid64").empty()) {
    set = FromWebApi(config, appid, language);
  } else {
    return Err("no_local_achievements",
               "Steam hasn't stored this game's achievements on this computer",
               "Play it once through Steam, or add a Steam Web API key and your Steam ID to read "
               "them from Steam's servers.",
               Fix::Setting("steam.web_api_key"));
  }
  if (set) AddRarity(*set, appid);
  return set;
}

}  // namespace mira::steam
