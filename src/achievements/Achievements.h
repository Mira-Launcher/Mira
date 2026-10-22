#pragma once

#include <cstdint>
#include <filesystem>
#include <json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"

namespace mira::achievements {

// One achievement as its store describes it, in the store's order and with its meanings.
struct Achievement {
  std::string key;  // the store's id for it
  std::string name;
  std::string description;
  std::string icon_unlocked;  // URLs; either may be empty
  std::string icon_locked;
  bool hidden = false;  // its store keeps it secret until unlocked
  bool unlocked = false;
  std::int64_t unlocked_at = 0;  // unix seconds, 0 while locked or when the store doesn't say
  double rarity = -1;            // percent of players who have it, -1 unknown
  std::int64_t xp = 0;           // Epic's points, 0 elsewhere
};

struct GameAchievements {
  std::string source;  // "steam", "gog" or "epic"
  std::int64_t fetched_at = 0;
  std::vector<Achievement> items;
};

nlohmann::json ToJson(const GameAchievements& set);
GameAchievements FromJson(const nlohmann::json& json);
// What GET /v1/games/{id}/achievements answers: counts, and each item with an icon version in
// place of its URLs.
nlohmann::json ApiJson(const GameAchievements& set);
// Whether two fetches differ in anything a client shows.
bool SameContent(const GameAchievements& a, const GameAchievements& b);

// The language achievements are asked for in, from LC_ALL, LC_MESSAGES or LANG; English when
// unset or C.
struct Locale {
  std::string language = "en";  // ISO 639-1, lower case
  std::string region = "US";    // ISO 3166-1, upper case; may be empty
};
Locale SystemLocale();
// "en-US", as GOG and Epic take it.
std::string LanguageTag(const Locale& locale);
// Steam's name for the language ("english", "german", "schinese").
std::string SteamLanguage(const Locale& locale);

// A store endpoint's base URL: `fallback`, or $MIRA_<NAME>_URL when set (tests serve fixtures
// from file:// that way).
std::string Endpoint(std::string_view name, std::string_view fallback);
// Whether `url` may be fetched with curl: http(s), or file:// under an Endpoint override.
bool FetchableUrl(std::string_view url);

// Whether `game`'s source keeps achievements Mira can read.
bool HasSource(const model::Game& game);
// Fetches `game`'s achievements from its store. Err no_achievements_source for a source without
// them, or the store's own error (signed out, no local files).
Result<GameAchievements> Fetch(const config::Config& config, const model::Game& game);

// Where an icon URL is kept once downloaded: `dir`/<hash of url>.jpg or .png.
std::optional<std::filesystem::path> CachedIcon(const std::filesystem::path& dir,
                                                std::string_view url);
// Downloads `url` into `dir`, fitted to 128 px, and returns the file.
Result<std::filesystem::path> DownloadIcon(const std::filesystem::path& dir,
                                           const std::string& url);
// Deletes icons in `dir` that no item of `set` refers to any more.
void PruneIcons(const std::filesystem::path& dir, const GameAchievements& set);

}  // namespace mira::achievements
