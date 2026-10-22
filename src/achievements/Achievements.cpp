#include "achievements/Achievements.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <set>
#include <unordered_set>

#include "core/AtomicFile.h"
#include "core/Command.h"
#include "core/Json.h"
#include "core/Strings.h"
#include "epic/Achievements.h"
#include "gog/Achievements.h"
#include "metadata/ImageFit.h"
#include "runner/Exec.h"
#include "steam/Achievements.h"

namespace mira::achievements {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

// Endpoints a test may move, by the name its $MIRA_<NAME>_URL uses.
constexpr std::string_view kEndpoints[] = {"STEAM_API", "STEAM_ICONS", "GOG_GAMEPLAY", "GOG_EMBED"};

using strings::StableHash;

std::string IconVersion(const Achievement& item) {
  if (item.icon_unlocked.empty() && item.icon_locked.empty()) return {};
  return StableHash(item.icon_unlocked + "\n" + item.icon_locked).substr(0, 12);
}

// A key is one achievement: one listed twice would show twice and count twice.
void KeepFirstOfEachKey(std::vector<Achievement>& items) {
  std::unordered_set<std::string> seen;
  std::erase_if(items, [&](const Achievement& item) { return !seen.insert(item.key).second; });
}

}  // namespace

json ToJson(const GameAchievements& set) {
  json items = json::array();
  for (const Achievement& item : set.items) {
    items.push_back({{"key", item.key},
                     {"name", item.name},
                     {"description", item.description},
                     {"icon_unlocked", item.icon_unlocked},
                     {"icon_locked", item.icon_locked},
                     {"hidden", item.hidden},
                     {"unlocked", item.unlocked},
                     {"unlocked_at", item.unlocked_at},
                     {"rarity", item.rarity},
                     {"xp", item.xp}});
  }
  return {{"source", set.source}, {"fetched_at", set.fetched_at}, {"items", std::move(items)}};
}

GameAchievements FromJson(const json& in) {
  GameAchievements set;
  set.source = core::JsonString(in, "source");
  set.fetched_at = core::JsonInt(in, "fetched_at");
  const json items = in.is_object() ? in.value("items", json::array()) : json::array();
  if (!items.is_array()) return set;
  for (const json& entry : items) {
    if (!entry.is_object()) continue;
    Achievement item;
    item.key = core::JsonString(entry, "key");
    item.name = core::JsonString(entry, "name");
    item.description = core::JsonString(entry, "description");
    item.icon_unlocked = core::JsonString(entry, "icon_unlocked");
    item.icon_locked = core::JsonString(entry, "icon_locked");
    item.hidden = core::JsonBool(entry, "hidden");
    item.unlocked = core::JsonBool(entry, "unlocked");
    item.unlocked_at = core::JsonInt(entry, "unlocked_at");
    const auto rarity = entry.find("rarity");
    item.rarity = rarity != entry.end() && rarity->is_number() ? rarity->get<double>() : -1;
    item.xp = core::JsonInt(entry, "xp");
    set.items.push_back(std::move(item));
  }
  // Also on read, so a cache written with repeats shows each once.
  KeepFirstOfEachKey(set.items);
  return set;
}

json ApiJson(const GameAchievements& set) {
  json items = json::array();
  int unlocked = 0;
  for (const Achievement& item : set.items) {
    if (item.unlocked) ++unlocked;
    items.push_back({{"key", item.key},
                     {"name", item.name},
                     {"description", item.description},
                     {"hidden", item.hidden},
                     {"unlocked", item.unlocked},
                     {"unlocked_at", item.unlocked_at},
                     {"rarity", item.rarity},
                     {"xp", item.xp},
                     {"icon", IconVersion(item)}});
  }
  return {{"source", set.source},
          {"fetched_at", set.fetched_at},
          {"total", set.items.size()},
          {"unlocked", unlocked},
          {"items", std::move(items)}};
}

bool SameContent(const GameAchievements& a, const GameAchievements& b) {
  json left = ToJson(a);
  json right = ToJson(b);
  left.erase("fetched_at");
  right.erase("fetched_at");
  return left == right;
}

Locale SystemLocale() {
  std::string value;
  for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    if (const char* set = std::getenv(name); set != nullptr && *set != '\0') {
      value = set;
      break;
    }
  }
  // "de_DE.UTF-8@euro" -> "de", "DE"
  value = value.substr(0, value.find_first_of(".@"));
  Locale locale;
  const std::string language = strings::ToLower(value.substr(0, value.find('_')));
  if (language.size() < 2 || language.size() > 3 ||
      !std::ranges::all_of(language, [](unsigned char c) { return std::isalpha(c); }) ||
      language == "c" || language == "posix")
    return locale;
  locale.language = language;
  locale.region.clear();
  if (const std::size_t underscore = value.find('_'); underscore != std::string::npos) {
    std::string region = value.substr(underscore + 1);
    if (region.size() == 2) {
      std::ranges::transform(region, region.begin(),
                             [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      locale.region = std::move(region);
    }
  }
  return locale;
}

std::string LanguageTag(const Locale& locale) {
  return locale.region.empty() ? locale.language : locale.language + "-" + locale.region;
}

std::string SteamLanguage(const Locale& locale) {
  const std::string& lang = locale.language;
  const std::string& region = locale.region;
  if (lang == "zh") return region == "TW" || region == "HK" ? "tchinese" : "schinese";
  if (lang == "pt") return region == "BR" ? "brazilian" : "portuguese";
  if (lang == "es") return region.empty() || region == "ES" ? "spanish" : "latam";
  static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
      {"ar", "arabic"},    {"bg", "bulgarian"}, {"cs", "czech"},      {"da", "danish"},
      {"de", "german"},    {"el", "greek"},     {"en", "english"},    {"fi", "finnish"},
      {"fr", "french"},    {"hu", "hungarian"}, {"id", "indonesian"}, {"it", "italian"},
      {"ja", "japanese"},  {"ko", "koreana"},   {"nb", "norwegian"},  {"nl", "dutch"},
      {"no", "norwegian"}, {"pl", "polish"},    {"ro", "romanian"},   {"ru", "russian"},
      {"sv", "swedish"},   {"th", "thai"},      {"tr", "turkish"},    {"uk", "ukrainian"},
      {"vi", "vietnamese"}};
  for (const auto& [code, name] : kNames) {
    if (code == lang) return std::string(name);
  }
  return "english";
}

std::int64_t ParseTime(std::string_view text) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  char separator = 0;
  int read = 0;
  const std::string copy(text);
  if (std::sscanf(copy.c_str(), "%4d-%2u-%2u%c%2d:%2d:%2d%n", &year, &month, &day, &separator,
                  &hour, &minute, &second, &read) != 7 ||
      (separator != 'T' && separator != ' '))
    return 0;
  const std::chrono::year_month_day date{std::chrono::year(year), std::chrono::month(month),
                                         std::chrono::day(day)};
  if (!date.ok()) return 0;
  std::int64_t seconds = std::chrono::sys_days(date).time_since_epoch() / std::chrono::seconds(1) +
                         hour * 3600 + minute * 60 + second;
  // A fraction, then Z or an offset ("+0000", "+00:00").
  std::string_view rest = text.substr(static_cast<std::size_t>(read));
  while (!rest.empty() &&
         (rest.front() == '.' || std::isdigit(static_cast<unsigned char>(rest.front()))))
    rest.remove_prefix(1);
  if (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) {
    const int sign = rest.front() == '+' ? 1 : -1;
    std::string digits;
    for (const char c : rest.substr(1)) {
      if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
    }
    if (digits.size() == 4) {
      seconds -= sign * (std::stoi(digits.substr(0, 2)) * 3600 + std::stoi(digits.substr(2)) * 60);
    }
  }
  return seconds;
}

std::string Endpoint(std::string_view name, std::string_view fallback) {
  const std::string variable = std::format("MIRA_{}_URL", name);
  if (const char* set = std::getenv(variable.c_str()); set != nullptr && *set != '\0') return set;
  return std::string(fallback);
}

bool FetchableUrl(std::string_view url) {
  if (url.starts_with("https://") || url.starts_with("http://")) return true;
  if (!url.starts_with("file://")) return false;
  return std::ranges::any_of(kEndpoints, [&](std::string_view name) {
    const std::string base = Endpoint(name, "");
    return !base.empty() && base.starts_with("file://") && url.starts_with(base);
  });
}

bool HasSource(const model::Game& game) {
  return game.runner_ref.starts_with("steam:") ||
         ((game.source == "gog" || game.source == "epic") && !game.source_ref.empty());
}

Result<GameAchievements> Fetch(const config::Config& config, const model::Game& game) {
  const Locale locale = SystemLocale();
  Result<GameAchievements> fetched;
  if (game.runner_ref.starts_with("steam:")) {
    fetched = steam::FetchAchievements(config, game.runner_ref.substr(6), locale);
  } else if (game.source == "gog" && !game.source_ref.empty()) {
    fetched = gog::FetchAchievements(config, game.source_ref, locale);
  } else if (game.source == "epic" && !game.source_ref.empty()) {
    fetched = epic::FetchAchievements(config, game.source_ref);
  } else {
    return Err("no_achievements_source",
               std::format("Mira can't read achievements for \"{}\"", game.name),
               "Achievements are shown for Steam, GOG and Epic games.");
  }
  if (!fetched) return fetched;
  fetched->fetched_at = model::NowSeconds();
  KeepFirstOfEachKey(fetched->items);
  return fetched;
}

std::optional<fs::path> CachedIcon(const fs::path& dir, std::string_view url) {
  // DownloadIcon's only two names, looked up rather than listing a folder of hundreds per icon.
  const std::string stem = StableHash(url);
  std::error_code ec;
  for (const char* ext : {".jpg", ".png"}) {
    fs::path file = dir / (stem + ext);
    if (fs::is_regular_file(file, ec)) return file;
  }
  return std::nullopt;
}

Result<fs::path> DownloadIcon(const fs::path& dir, const std::string& url) {
  if (!FetchableUrl(url)) return Err("invalid_icon_url", "the store gave no usable icon address");
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return Err("icon_write_failed", ec.message(), kDiskHint);
  static std::atomic<unsigned> next_part{0};
  const std::string stem = StableHash(url);
  const fs::path part = dir / std::format("{}.{}.part", stem, next_part++);
  Command command;
  command.argv = {"curl",
                  "-sSL",
                  "-f",
                  "--proto",
                  "=https,http,file",
                  "--max-time",
                  "20",
                  "--max-filesize",
                  "4M",
                  "-o",
                  part.string(),
                  "--url",
                  strings::EscapeUrl(url)};
  const auto result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) {
    fs::remove(part, ec);
    return Err("icon_download_failed", std::format("couldn't download {}", url), kConnectionHint);
  }
  if (metadata::FileContentType(part) == "application/octet-stream" || metadata::IsTooLarge(part)) {
    fs::remove(part, ec);
    return Err("icon_download_failed", std::format("{} isn't an image", url));
  }
  fs::path dest = dir / (stem + ".jpg");
  if (const auto fitted = metadata::FitImage(part, {.width = 128, .height = 128})) {
    if (auto written = WriteFileAtomic(part, fitted->bytes, "icon_write_failed"); !written) {
      fs::remove(part, ec);
      return std::unexpected(written.error());
    }
    dest.replace_extension(fitted->ext);
  } else if (metadata::FileContentType(part) == "image/png") {
    dest.replace_extension(".png");
  }
  fs::rename(part, dest, ec);
  if (ec) {
    fs::remove(part, ec);
    return Err("icon_write_failed", ec.message(), kDiskHint);
  }
  return dest;
}

void PruneIcons(const fs::path& dir, const GameAchievements& set) {
  std::set<std::string> kept;
  for (const Achievement& item : set.items) {
    for (const std::string* url : {&item.icon_unlocked, &item.icon_locked}) {
      if (!url->empty()) kept.insert(StableHash(*url));
    }
  }
  std::error_code ec;
  std::vector<fs::path> dropped;
  for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
    // A download still being written; it renames itself into place.
    if (entry.path().extension() == ".part") continue;
    if (!kept.contains(entry.path().stem().string())) dropped.push_back(entry.path());
  }
  for (const fs::path& path : dropped) fs::remove(path, ec);
}

}  // namespace mira::achievements
