#include "gog/Achievements.h"

#include <format>
#include <fstream>
#include <json.hpp>

#include "core/Json.h"
#include "gog/Gog.h"
#include "runner/Exec.h"

namespace mira::gog {
namespace {
using achievements::Achievement;
using achievements::GameAchievements;
using nlohmann::json;

Result<json> GetList(const std::string& url, const std::string& token, const std::string& locale) {
  const auto result = runner::RunCurlWithSecrets(
      {"-sSL", "-f", "--max-time", "15", "-H", "X-Gog-Lc: " + locale, url},
      runner::CurlConfigLine("header", "Authorization: Bearer " + token));
  if (!result) return std::unexpected(result.error());
  const json parsed = json::parse(result->output, nullptr, false);
  if (result->exit_code != 0 || !parsed.is_object()) {
    return Err("gog_api_error", "GOG didn't return the game's achievements", kConnectionHint);
  }
  return parsed;
}

}  // namespace

std::string UserId(const config::Config& config) {
  std::ifstream file(AuthConfigPath(config));
  const json parsed = json::parse(file, nullptr, false);
  if (!parsed.is_object() || parsed.empty() || !parsed.begin()->is_object()) return {};
  const json& stored = *parsed.begin();
  const auto id = stored.find("user_id");
  if (id == stored.end()) return {};
  if (id->is_string()) return id->get<std::string>();
  if (id->is_number_unsigned()) return std::to_string(id->get<std::uint64_t>());
  return {};
}

Result<GameAchievements> FetchAchievements(const config::Config& config,
                                           const std::string& product_id,
                                           const achievements::Locale& locale) {
  if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
  const auto token = AccessToken(config);
  if (!token) return std::unexpected(token.error());
  const std::string user = UserId(config);
  if (user.empty())
    return Err("not_authenticated", "GOG's sign-in has no account id", "",
               Fix::Source("gog", "login"));
  const std::string base = std::format(
      "{}/clients/{}/users/{}/achievements",
      achievements::Endpoint("GOG_GAMEPLAY", "https://gameplay.gog.com"), product_id, user);
  // One request gives the whole list (limit 1000), as comet and Heroic read it. Its `page_token`
  // is set on every reply and isn't a next page: following it fetched the same list again.
  const auto answer = GetList(base, *token, achievements::LanguageTag(locale));
  if (!answer) return std::unexpected(answer.error());
  GameAchievements set{.source = "gog", .fetched_at = 0, .items = {}};
  const json items = answer->value("items", json());
  if (!items.is_array()) return set;
  for (const json& entry : items) {
    Achievement item;
    item.key = core::JsonString(entry, "achievement_key");
    if (item.key.empty()) item.key = core::JsonString(entry, "achievement_id");
    if (item.key.empty()) continue;
    item.name = core::JsonString(entry, "name");
    item.description = core::JsonString(entry, "description");
    item.icon_unlocked = core::JsonString(entry, "image_url_unlocked");
    item.icon_locked = core::JsonString(entry, "image_url_locked");
    item.hidden = !core::JsonBool(entry, "visible", true);
    const std::string unlocked = core::JsonString(entry, "date_unlocked");
    item.unlocked = !unlocked.empty();
    item.unlocked_at = achievements::ParseTime(unlocked);
    if (const auto rarity = entry.find("rarity"); rarity != entry.end() && rarity->is_number())
      item.rarity = rarity->get<double>();
    set.items.push_back(std::move(item));
  }
  return set;
}

}  // namespace mira::gog
