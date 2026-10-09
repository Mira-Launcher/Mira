#include "metadata/MetadataFetcher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <thread>
#include <string_view>

#include <json.hpp>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "amazon/Nile.h"
#include "core/AtomicFile.h"
#include "core/Command.h"
#include "core/Lane.h"
#include "config/Resolver.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "epic/Legendary.h"
#include "lutris/LutrisImporter.h"
#include "metadata/ImageFit.h"
#include "runner/Exec.h"

namespace mira::metadata {
namespace {

// Raised when details gain a field worth fetching again for: 2 has Steam's trailer streams.
constexpr int kDetailsVersion = 2;
namespace fs = std::filesystem;
using nlohmann::json;

std::unexpected<Error> NoGriddbKey(std::string message) {
  return Err("no_steamgriddb_key", std::move(message),
             "Add a free SteamGridDB API key. Steam games don't need one.", Fix::Setting("steamgriddb.api_key"));
}

// Every network call gets a hard ceiling: this runs unattended off a scan,
// not a user-triggered download, so an unreachable or hanging endpoint must
// never pile up a stuck background thread.
constexpr std::string_view kMaxTime = "10";

// Reads one optional field, treating "absent" and "present but null" the
// same way.
//
// This is not defensive decoration. nlohmann's value() throws
// type_error.302 when the key exists holding a different type, and JSON
// null is a different type, so `data.value("website", std::string())`
// throws on every Steam store page that has no website. Neon White,
// HoloCure and Armored Core VI are all such pages, and all three threw out
// of Fetch and ended up with no metadata and no cover at all. Every field
// read from a source we do not control goes through here.
template <typename T>
T Value(const json& object, const char* key, T fallback) {
  if (!object.is_object()) return fallback;
  const auto entry = object.find(key);
  if (entry == object.end() || entry->is_null()) return fallback;
  try {
    return entry->get<T>();
  } catch (const json::exception&) {
    // A field of an unexpected type is the source's problem, not a reason
    // to lose the rest of the record.
    return fallback;
  }
}

std::string UrlEncode(std::string_view input) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(input.size());
  for (unsigned char c : input) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

// Runs curl and parses its stdout as JSON. Empty/malformed output (offline,
// rate-limited, endpoint down) comes back as a discarded json rather than an
// error: every call site treats "couldn't get this source" as "skip it",
// not "fail the whole fetch".
json CurlJson(std::vector<std::string> argv) {
  argv.insert(argv.begin() + 1, {"--max-time", std::string(kMaxTime), "--retry", "2", "--retry-max-time", "30"});
  Command command;
  command.argv = std::move(argv);
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) return json();
  return json::parse(result->output, nullptr, false);
}

std::string ContentTypeFor(const fs::path& file) {
  const std::string ext = strings::ToLower(file.extension().string());
  if (ext == ".png") return "image/png";
  if (ext == ".webp") return "image/webp";
  return "image/jpeg";
}

Box ArtBox(const config::Config& config, std::string_view slot, bool title = false) {
  return SlotBox(slot, title, config.GetString("metadata.art_size") == "compact");
}

// Fits the finished `part` into `box` and moves it to `dest`, whose extension follows the fitted
// image's, then drops the slot's files of another type (.png to .jpg).
bool CommitArt(const fs::path& part, fs::path& dest, Box box) {
  if (const auto fitted = FitImage(part, box)) {
    if (!WriteFileAtomic(part, fitted->bytes, "artwork_write_failed")) return false;
    dest.replace_extension(fitted->ext);
  }
  std::error_code ec;
  fs::rename(part, dest, ec);
  if (ec) return false;
  const std::string stem = dest.stem().string();
  for (const auto& entry : fs::directory_iterator(dest.parent_path(), ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.path() != dest && entry.path().stem() == stem && !name.ends_with(".part")) {
      std::error_code remove_ec;
      fs::remove(entry.path(), remove_ec);
    }
  }
  return true;
}

// Downloads `url` into this game's artwork dir under `slot` (curl -f, so a
// 404 never gets saved as art), recording it into
// `info[slot == "cover" ? "artwork" : slot]` on success; "artwork" is
// legacy naming for the cover slot, kept for API wire compatibility.
// Sends no credentials: both sources use a plain public CDN, and
// SteamGridDB's own API 401s if its key is passed to it here.
//
// `candidate_id`, when the image came from one of art_candidates[slot],
// records which one -- the one piece of state a caller needs to show which
// candidate is the one currently active for a slot.
bool FetchArtworkInto(const config::Config& config, const std::string& url,
                      const std::string& game_id, std::string_view source, std::string_view slot,
                      json& info, std::optional<std::int64_t> candidate_id = std::nullopt,
                      bool picked = false) {
  const std::string key = slot == "cover" ? "artwork" : std::string(slot);
  // A slot the user picked by hand (SelectArtwork) stays until they pick again.
  if (info.contains(key) && Value(info[key], "chosen", false)) return true;
  std::string ext = fs::path(std::string(url)).extension().string();
  if (ext.empty() || ext.size() > 5) ext = ".jpg";

  const fs::path dir = ArtworkDir(config, game_id);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    log::Warn("couldn't create artwork dir for {}: {}", game_id, ec.message());
    return false;
  }
  // A pick has a file of its own, so a refresh already downloading the automatic choice can't
  // overwrite it.
  const std::string stem = picked ? std::format("{}.chosen", slot) : std::string(slot);
  fs::path dest = dir / (stem + ext);
  // Beside it until complete: a failed download must not take the slot's
  // current image with it.
  static std::atomic<unsigned> next_part{0};
  const fs::path part = dir / std::format("{}{}.part{}", slot, ext, next_part++);

  Command command;
  // A URL from a store's JSON is http(s) only. Lutris's cached art is a file:// URL we built, and a
  // SteamGridDB candidate was checked as http(s) when it was recorded.
  const bool trusted = source == "lutris" || source == "steamgriddb";
  command.argv = {"curl", "-sSL", "-f", "--retry", "2", "--retry-max-time", "30", "--proto",
                  trusted ? "=file,https,http" : "=https,http", "--max-time",
                  std::string(kMaxTime), "-o", part.string(), "--url", url};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0 || !CommitArt(part, dest, ArtBox(config, slot))) {
    log::Warn("couldn't download artwork for {} from {}", game_id, url);
    fs::remove(part, ec);
    return false;
  }
  info[key] = {{"file", dest.filename().string()}, {"content_type", ContentTypeFor(dest)},
              {"source", source}};
  if (candidate_id) info[key]["candidate_id"] = *candidate_id;
  return true;
}

// Fits a slot's saved file into `box`, renaming it when its type changes.
void RefitSlot(const fs::path& dir, json& slot, Box box) {
  const std::string name = Value(slot, "file", std::string());
  if (name.empty() || name.find('/') != std::string::npos) return;
  const fs::path file = dir / name;
  const auto fitted = FitImage(file, box);
  if (!fitted) return;
  fs::path dest = file;
  dest.replace_extension(fitted->ext);
  if (!WriteFileAtomic(dest, fitted->bytes, "artwork_write_failed")) return;
  std::error_code ec;
  if (dest != file) fs::remove(file, ec);
  slot["file"] = dest.filename().string();
  slot["content_type"] = ContentTypeFor(dest);
}

// Fetches every SteamGridDB candidate for one art slot (grids/heroes/logos/
// icons -> cover/hero/logo/icon) and stores the full list in
// info["art_candidates"][slot] so a caller can offer a choice. Downloads the
// first that isn't adult art (SteamGridDB ranks them) as the slot's default only if
// the slot doesn't already have one -- a Steam-owned game already got its
// cover/hero from Steam's own CDN (FetchSteamOwned), and SteamGridDB here is
// an alternate to switch to, not a replacement to switch to automatically.
// One page (50 results) of a SteamGridDB game's art for one endpoint.
json FetchGriddbPage(const config::Config& config, const std::string& auth_header, std::string_view endpoint,
                     std::int64_t griddb_id, int page) {
  // SteamGridDB leaves adult art out unless asked for it.
  const std::string_view nsfw = config.GetBool("steamgriddb.nsfw") ? "any" : "false";
  // "Grids" include Steam's wide 920x430 capsules; a cover is the tall kind.
  const std::string_view dimensions = endpoint == "grids" ? "&dimensions=600x900,342x482,660x930" : "";
  return CurlJson({"curl", "-sSL", "-H", auth_header,
                   std::format("https://www.steamgriddb.com/api/v2/{}/game/{}?nsfw={}&page={}{}", endpoint, griddb_id,
                               nsfw, page, dimensions)});
}

// A SteamGridDB result as an art_candidates entry; null without a url.
json GriddbCandidate(const json& item) {
  const std::string url = Value(item, "url", std::string());
  if (!url.starts_with("https://") && !url.starts_with("http://")) return nullptr;
  const std::string thumb = Value(item, "thumb", std::string());
  return {
      {"id", Value(item, "id", std::int64_t{0})},
      {"url", url},
      {"thumb", thumb.starts_with("http") ? thumb : std::string()},
      {"width", Value(item, "width", 0)},
      {"height", Value(item, "height", 0)},
      {"style", Value(item, "style", std::string())},
      {"nsfw", Value(item, "nsfw", false)},
      {"source", "steamgriddb"},
  };
}

// A thread for one of a fetch's requests. An exception is logged: thrown out of a thread, it
// would end mirad.
std::jthread Spawn(std::function<void()> body) {
  return std::jthread([body = std::move(body)] {
    try {
      body();
    } catch (const std::exception& error) {
      log::Warn("a metadata request threw: {}", error.what());
    }
  });
}

// Every read-modify-write of a game's metadata file after its fetch.
std::mutex& MetadataFileMutex() {
  static std::mutex mutex;
  return mutex;
}

void FetchGriddbSlot(const config::Config& config, const std::string& auth_header, std::int64_t griddb_id,
                     const std::string& game_id, std::string_view endpoint, std::string_view slot, json& info) {
  // Only the first page: the art picker asks for more as it scrolls.
  const json response = FetchGriddbPage(config, auth_header, endpoint, griddb_id, 0);
  if (response.is_discarded() || !Value(response, "success", false)) return;
  const json data = Value(response, "data", json::array());
  if (data.empty()) return;

  // Appended, not assigned: a Steam-owned game seeds this slot with its own
  // CDN image first, and that entry must survive so the picker can switch
  // back to it, not just to a SteamGridDB alternate.
  json candidates = (info.contains("art_candidates") && info["art_candidates"].contains(std::string(slot)))
                        ? info["art_candidates"][std::string(slot)]
                        : json::array();
  bool found_any = false;
  for (const auto& item : data) {
    json candidate = GriddbCandidate(item);
    if (candidate.is_null()) continue;
    candidates.push_back(std::move(candidate));
    found_any = true;
  }
  if (!found_any) return;
  info["art_candidates"][std::string(slot)] = candidates;

  const std::string key = slot == "cover" ? "artwork" : std::string(slot);
  // Only missing here if the Steam CDN download failed -- no Steam entry
  // was seeded above either then, so candidates[0] is SteamGridDB's own.
  if (info.contains(key)) return;

  // The top result that isn't adult art: that is only ever picked by hand.
  const auto best = std::ranges::find_if(candidates, [](const json& candidate) { return !Value(candidate, "nsfw", false); });
  if (best == candidates.end()) return;
  // No credentials on the image download itself -- see FetchArtworkInto.
  const std::string best_url = Value(*best, "url", std::string());
  const std::int64_t best_id = Value(*best, "id", std::int64_t{0});
  FetchArtworkInto(config, best_url, game_id, "steamgriddb", slot, info, best_id);
}

// Resolves `name` to a SteamGridDB game id and fetches every candidate for
// every slot. Best-effort and silent about it: called for both a game that
// has nothing else (FetchNonSteam, where the caller turns a missing key into
// a hard error beforehand) and one that already has Steam's own art
// (FetchSteamOwned, where a missing key or no name match just means no
// alternates to offer).
// SteamGridDB's top match for `name`, or 0.
std::int64_t FindGriddbId(const std::string& auth_header, const std::string& name) {
  const json search = CurlJson({"curl", "-sSL", "-H", auth_header,
                                std::format("https://www.steamgriddb.com/api/v2/search/autocomplete/{}",
                                           UrlEncode(name))});
  if (search.is_discarded() || !Value(search, "success", false) || Value(search, "data", json::array()).empty()) {
    return 0;
  }
  return Value(search["data"][0], "id", std::int64_t{0});
}

void FetchGriddbCandidates(const config::Config& config, const std::string& api_key, const std::string& name,
                           const std::string& game_id, std::int64_t chosen_id, json& info) {
  const std::string auth_header = std::format("Authorization: Bearer {}", api_key);
  // metadata.steamgriddb_id, when the top match was wrong
  const std::int64_t griddb_id = chosen_id != 0 ? chosen_id : FindGriddbId(auth_header, name);
  if (griddb_id == 0) return;
  info["steamgriddb_id"] = griddb_id;

  // Every candidate for every slot goes into info["art_candidates"][slot]
  // (see FetchGriddbSlot) so a caller can offer a choice instead of only
  // ever getting SteamGridDB's top pick.
  // The four slots at once, each on a copy holding only its own keys.
  constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kEndpoints = {
      {{"grids", "cover"}, {"heroes", "hero"}, {"logos", "logo"}, {"icons", "icon"}}};
  std::array<json, 4> parts;
  {
    std::vector<std::jthread> threads;
    for (std::size_t i = 0; i < kEndpoints.size(); ++i) {
      const auto [endpoint, slot] = kEndpoints[i];
      const std::string key = slot == "cover" ? "artwork" : std::string(slot);
      parts[i] = json::object();
      if (info.contains(key)) parts[i][key] = info[key];
      if (info.contains("art_candidates") && info["art_candidates"].contains(std::string(slot))) {
        parts[i]["art_candidates"][std::string(slot)] = info["art_candidates"][std::string(slot)];
      }
      threads.push_back(Spawn([&, i, endpoint, slot] {
        FetchGriddbSlot(config, auth_header, griddb_id, game_id, endpoint, slot, parts[i]);
      }));
    }
  }
  for (const json& part : parts) {
    for (const auto& [key, value] : part.items()) {
      if (key == "art_candidates") {
        for (const auto& [slot, list] : value.items()) info["art_candidates"][slot] = list;
      } else {
        info[key] = value;
      }
    }
  }
}

// GET protondb's own reports summary for a Steam AppID. Best-effort and
// silent about it -- see FetchGriddbCandidates's own comment for why "no
// data for this id" isn't treated as an error here either.
void FetchProtonDb(const std::string& appid, json& info) {
  const json proton =
      CurlJson({"curl", "-sSL", std::format("https://www.protondb.com/api/v1/reports/summaries/{}.json", appid)});
  if (!proton.is_discarded() && proton.contains("tier")) {
    info["protondb"] = {{"tier", Value(proton, "tier", std::string())},
                        {"confidence", Value(proton, "confidence", std::string())},
                        {"total_reports", Value(proton, "total", 0)}};
  }
}

// Best-effort Steam AppID lookup by name, so a non-Steam game (Lutris,
// scanned, manually added) can still get a ProtonDB tier -- ProtonDB is
// keyed by AppID and this is the only source of one Mira has for a game
// that isn't runner_ref="steam:<appid>" already. Never touches runner_ref
// or how the game actually launches; it only feeds FetchProtonDb below.
// Steam's own search ranks relevance server-side, same trust level this
// file already gives SteamGridDB's own top autocomplete result
// (FetchGriddbCandidates) -- a generic title can still match the wrong
// game, which is the accepted tradeoff of matching by name at all.
//
// One search gives both answers: `best` is Steam's top app, for ProtonDB, and
// `exact` the first whose name matches `name` ignoring case, spaces and
// punctuation, for art, where a wrong match shows.
struct SteamMatch {
  std::string best;
  std::string exact;
};

SteamMatch FindSteamAppIds(const std::string& name) {
  const auto normalize = [](std::string_view text) {
    std::string out;
    for (const unsigned char c : text) {
      if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
    }
    return out;
  };
  // A scanned game's name is its folder's: "CloneDroneintheDangerZone" or
  // "Hollow_Knight" finds nothing until split into words.
  std::string term;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = name[i];
    if (c == '_' || c == '.' || c == '-') {
      term += ' ';
      continue;
    }
    if (i > 0 && std::isupper(c) && std::islower(static_cast<unsigned char>(name[i - 1]))) term += ' ';
    term += static_cast<char>(c);
  }
  const json search = CurlJson(
      {"curl", "-sSL", std::format("https://store.steampowered.com/api/storesearch/?term={}&l=english&cc=us",
                                   UrlEncode(term))});
  SteamMatch match;
  if (search.is_discarded()) return match;
  for (const auto& item : Value(search, "items", json::array())) {
    // "app" (games, DLC, demos), not "sub"/"bundle". It was checked against
    // "game", which Steam never sends, so this never matched anything.
    if (Value(item, "type", std::string()) != "app") continue;
    const std::int64_t id = Value(item, "id", std::int64_t{0});
    if (id == 0) continue;
    if (match.best.empty()) match.best = std::to_string(id);
    if (normalize(Value(item, "name", std::string())) == normalize(name)) {
      match.exact = std::to_string(id);
      break;
    }
  }
  return match;
}

// A game's Steam apps by name, searched at most once. metadata.steam_appid, when set, is both.
class SteamMatcher {
public:
  SteamMatcher(const config::Config& config, const model::Game& game)
      : name_(game.name),
        fixed_(config::Resolver(config, game.overrides).GetInt("metadata.steam_appid")) {}
  const SteamMatch& operator()() {
    if (!match_) {
      match_ = fixed_ != 0 ? SteamMatch{std::to_string(fixed_), std::to_string(fixed_)} : FindSteamAppIds(name_);
    }
    return *match_;
  }

private:
  std::string name_;
  std::int64_t fixed_;
  std::optional<SteamMatch> match_;
};

// One of Epic's keyImages from Legendary's cached metadata. Epic names the
// same art differently per title: most use DieselGameBox*, some
// DieselStoreFront* or OfferImage*. Tried in order.
constexpr std::string_view kEpicCoverTypes[] = {"DieselGameBoxTall", "DieselStoreFrontTall", "OfferImageTall",
                                                 "Thumbnail"};
constexpr std::string_view kEpicHeroTypes[] = {"DieselGameBox", "DieselStoreFrontWide", "OfferImageWide"};

std::string FindKeyImage(const json& key_images, std::span<const std::string_view> types) {
  for (std::string_view type : types) {
    for (const auto& image : key_images) {
      if (Value(image, "type", std::string()) == type) return Value(image, "url", std::string());
    }
  }
  return {};
}

// One GetItems call for many Steam apps: assets, and with `details` the store info, review summary
// and the 20 tags Steam shows too. store_items in the order asked, or an empty array. No key needed.
json SteamStoreItems(const std::vector<std::string>& appids, bool details) {
  json ids = json::array();
  for (const std::string& appid : appids) ids.push_back({{"appid", std::stoll(appid)}});
  json request = {{"include_assets", true}};
  if (details) {
    request.update({{"include_basic_info", true},
                    {"include_reviews", true},
                    {"include_release", true},
                    {"include_tag_count", 20}});
  }
  const json input = {{"ids", ids}, {"context", {{"language", "english"}, {"country_code", "US"}}},
                      {"data_request", request}};
  const json items = CurlJson(
      {"curl", "-sSL", "https://api.steampowered.com/IStoreBrowseService/GetItems/v1?input_json=" + UrlEncode(input.dump())});
  return Value(Value(items, "response", json::object()), "store_items", json::array());
}

// Steam's vertical library cover from a store item's assets. Newer apps keep their art under a
// hashed path, so the fixed library_600x900.jpg 404s for them.
std::string SteamCoverUrl(const std::string& appid, const json& item) {
  const json assets = Value(item, "assets", json::object());
  const std::string format = Value(assets, "asset_url_format", std::string());
  // library_capsule is 300x450 despite its 600x900 name; the 2x one is 600x900.
  std::string file = Value(assets, "library_capsule_2x", std::string());
  if (file.empty()) file = Value(assets, "library_capsule", std::string());
  if (format.empty() || file.empty()) {
    return std::format("https://cdn.akamai.steamstatic.com/steam/apps/{}/library_600x900_2x.jpg", appid);
  }
  std::string path = format;
  if (const std::size_t at = path.find("${FILENAME}"); at != std::string::npos) {
    path.replace(at, std::string_view("${FILENAME}").size(), file);
  }
  return "https://shared.akamai.steamstatic.com/store_item_assets/" + path;
}

std::string SteamCoverUrl(const std::string& appid) {
  const json items = SteamStoreItems({appid}, false);
  return SteamCoverUrl(appid, items.empty() ? json::object() : items[0]);
}

void AddStoreItemReviews(const json& item, json& info);

// A GetItems store item's info and review summary, shaped like FetchSteamDetails' so a reader
// can't tell them apart.
void AddStoreItemDetails(const std::string& appid, const json& item, json& info) {
  const json basic = Value(item, "basic_info", json::object());
  const auto names = [&basic](const char* key) {
    json out = json::array();
    for (const json& entry : Value(basic, key, json::array())) {
      if (std::string name = Value(entry, "name", std::string()); !name.empty()) out.push_back(std::move(name));
    }
    return out;
  };
  json steam = {{"appid", appid},
                {"short_description", Value(basic, "short_description", std::string())},
                {"developers", names("developers")},
                {"publishers", names("publishers")}};
  if (const std::int64_t released = Value(Value(item, "release", json::object()), "steam_release_date", std::int64_t{0});
      released > 0) {
    const std::time_t time = released;
    std::tm tm{};
    char text[32];
    if (gmtime_r(&time, &tm) && std::strftime(text, sizeof text, "%e %b, %Y", &tm) > 0) {
      steam["release_date"] = strings::Trim(text);
    }
  }
  info["steam"] = steam;
  AddStoreItemReviews(item, info);
}

// A GetItems store item's review summary, when it has one.
void AddStoreItemReviews(const json& item, json& info) {
  const json summary = Value(Value(item, "reviews", json::object()), "summary_filtered", json::object());
  if (const int total = Value(summary, "review_count", 0); total > 0) {
    const int positive = static_cast<int>(std::int64_t{total} * Value(summary, "percent_positive", 0) / 100);
    info["steam_reviews"] = {{"score_description", Value(summary, "review_score_label", std::string())},
                             {"total_positive", positive},
                             {"total_negative", total - positive},
                             {"total_reviews", total}};
  }
}

// One app's GetItems item with details, an empty object when Steam didn't answer for it.
json SteamStoreItem(const std::string& appid) {
  const json items = SteamStoreItems({appid}, true);
  return !items.empty() && Value(items[0], "success", 0) == 1 ? items[0] : json::object();
}

// Steam's tag ids to their English names. Kept beside the metadata, since the list barely changes,
// and asked for again when Steam names a tag id it doesn't hold.
std::map<std::int64_t, std::string> SteamTagNames(store::MetadataStore& cache,
                                                  const std::set<std::int64_t>& needed) {
  static std::mutex mutex;
  const std::lock_guard lock(mutex);
  constexpr const char* kList = "steam_tag_names";
  std::map<std::int64_t, std::string> names;
  const auto read = [&](const json& list) {
    for (const auto& tag : Value(Value(list, "response", json::object()), "tags", json::array())) {
      const std::int64_t id = Value(tag, "tagid", std::int64_t{0});
      const std::string name = Value(tag, "name", std::string());
      if (id != 0 && !name.empty()) names[id] = name;
    }
  };
  read(cache.ReadList(kList));
  if (std::ranges::all_of(needed, [&](std::int64_t id) { return names.contains(id); })) return names;
  const json list =
      CurlJson({"curl", "-sSL", "https://api.steampowered.com/IStoreService/GetTagList/v1/?language=english"});
  if (list.is_discarded()) return names;
  names.clear();
  read(list);
  if (auto written = cache.WriteList(kList, list); !written) {
    log::Warn("could not keep Steam's tag names: {}", written.error().message);
  }
  return names;
}

// info["steam_tags"] from `appid`'s GetItems item, most voted first: the app they came from (null
// when no Steam game matched) and its tags. Left out when Steam didn't answer, so it's asked again.
void AddStoreItemTags(store::MetadataStore& cache, const std::string& appid, const json& item, json& info) {
  if (!appid.empty() && Value(item, "success", 0) != 1) return;
  std::vector<std::int64_t> ids;
  for (const json& tag : Value(item, "tags", json::array())) ids.push_back(Value(tag, "tagid", std::int64_t{0}));
  const std::map<std::int64_t, std::string> names =
      ids.empty() ? std::map<std::int64_t, std::string>{} : SteamTagNames(cache, {ids.begin(), ids.end()});
  std::vector<std::string> tags;
  for (const std::int64_t id : ids) {
    if (const auto name = names.find(id); name != names.end()) tags.push_back(name->second);
  }
  info["steam_tags"] = {{"appid", appid.empty() ? json(nullptr) : json(appid)},
                        {"tags", tags},
                        {"fetched_at", model::NowSeconds()}};
}

// GOG Galaxy's games database: art for a release on any store it
// integrates with ("gog", "steam", "itch", "amazon", ...), keyed by that
// store's own id. Public, no key. Returns the image URL for `field`
// ("vertical_cover", "horizontal_artwork", "logo"), or empty.
std::string GamesDbImageUrl(std::string_view platform, const std::string& external_id, std::string_view field) {
  const json release = CurlJson({"curl", "-sSL",
                                 std::format("https://gamesdb.gog.com/platforms/{}/external_releases/{}", platform,
                                             UrlEncode(external_id))});
  std::string url = Value(Value(Value(release, "game", json::object()), std::string(field).c_str(), json::object()),
                          "url_format", std::string());
  if (url.empty()) return {};
  for (const auto& [token, value] : {std::pair{"{formatter}", ""}, std::pair{"{ext}", "jpg"}}) {
    if (const std::size_t at = url.find(token); at != std::string::npos) url.replace(at, std::strlen(token), value);
  }
  return url;
}

// gamesdb's name for a Mira source, where it has one.
std::string_view GamesDbPlatform(const std::string& source) {
  if (source == "gog" || source == "itch" || source == "steam" || source == "amazon") return source;
  return {};
}

// nile's cached library entry art for an Amazon product: square-ish, but
// better than nothing when gamesdb doesn't know the game.
std::string AmazonImageUrl(const std::string& product_id) {
  const json library = amazon::ReadNileFile("library.json");
  if (!library.is_array()) return {};
  for (const json& item : library) {
    const json product = Value(item, "product", json::object());
    if (Value(product, "id", std::string()) != product_id) continue;
    const json detail = Value(product, "productDetail", json::object());
    const json details = Value(detail, "details", json::object());
    for (const char* key : {"iconUrl", "logoUrl"}) {
      if (std::string url = Value(details, key, std::string()); !url.empty()) return url;
      if (std::string url = Value(detail, key, std::string()); !url.empty()) return url;
    }
  }
  return {};
}

// Legendary's cached catalog entry for a title, or null.
json LegendaryMeta(const std::string& app_name) {
  std::ifstream in(epic::LegendaryMetadataFile(app_name));
  const json parsed = in ? json::parse(in, nullptr, false) : json();
  if (parsed.is_discarded() || !parsed.is_object()) return nullptr;
  return Value(parsed, "metadata", json::object());
}

void AddEpicDetails(const std::string& app_name, const json& meta, json& info) {
  info["epic"] = {
      {"app_name", app_name},
      {"description", Value(meta, "description", std::string())},
      {"developer", Value(meta, "developer", std::string())},
  };
}

// A store's own cover for one of its games, from wherever it has one, into
// info's cover slot. Returns whether one landed.
// `steam_item`, when a batch already has the game's GetItems entry.
bool FetchStoreCover(const config::Config& config, const model::Game& game, json& info,
                     const json* steam_item = nullptr) {
  if (game.runner_ref.starts_with("steam:")) {
    const std::string appid = game.runner_ref.substr(std::string_view("steam:").size());
    const std::string url = steam_item ? SteamCoverUrl(appid, *steam_item) : SteamCoverUrl(appid);
    if (FetchArtworkInto(config, url, game.id, "steam_cdn", "cover", info)) return true;
  }
  if (game.source == "epic") {
    const json key_images = Value(LegendaryMeta(game.source_ref), "keyImages", json::array());
    if (const std::string url = FindKeyImage(key_images, kEpicCoverTypes);
        !url.empty() && FetchArtworkInto(config, url, game.id, "epic", "cover", info)) {
      return true;
    }
  }
  if (const std::string_view platform = GamesDbPlatform(game.source); !platform.empty() && !game.source_ref.empty()) {
    if (const std::string url = GamesDbImageUrl(platform, game.source_ref, "vertical_cover");
        !url.empty() && FetchArtworkInto(config, url, game.id, "gog_gamesdb", "cover", info)) {
      return true;
    }
  }
  if (game.source == "amazon") {
    if (const std::string url = AmazonImageUrl(game.source_ref);
        !url.empty() && FetchArtworkInto(config, url, game.id, "amazon", "cover", info)) {
      return true;
    }
  }
  return false;
}

// Steam's store page, review summary, tags and ProtonDB tier for `appid`, whose GetItems item is
// `item`. Returns whether the store answered, so a rate-limited try can be made again.
bool FetchSteamDetails(const config::Config& config, store::MetadataStore& cache, const std::string& appid,
                       const json& item, json& info) {
  json proton_info;
  std::jthread proton = Spawn([&] { FetchProtonDb(appid, proton_info); });
  const json store = CurlJson(
      {"curl", "-sSL", std::format("https://store.steampowered.com/api/appdetails?appids={}&l=english", appid)});
  if (!store.is_discarded() && store.contains(appid) && Value(store[appid], "success", false)) {
    const json data = Value(store[appid], "data", json::object());
    json steam_info = {
        {"appid", appid},
        {"short_description", Value(data, "short_description", std::string())},
        {"release_date", Value(Value(data, "release_date", json::object()), "date", std::string())},
        {"developers", Value(data, "developers", json::array())},
        {"publishers", Value(data, "publishers", json::array())},
        {"website", Value(data, "website", std::string())},
    };
    // Written only when the store actually gave a value, so a free game
    // reads as "no price" rather than as a price of "".
    if (const int score = Value(Value(data, "metacritic", json::object()), "score", 0); score > 0) {
      steam_info["metacritic_score"] = score;
    }
    if (const std::string price =
            Value(Value(data, "price_overview", json::object()), "final_formatted", std::string());
        !price.empty()) {
      steam_info["price"] = price;
    }
    json genres = json::array();
    for (const auto& genre : Value(data, "genres", json::array())) {
      const std::string description = Value(genre, "description", std::string());
      if (!description.empty()) genres.push_back(description);
    }
    steam_info["genres"] = genres;

    if (data.contains("pc_requirements") && data["pc_requirements"].is_object()) {
      steam_info["pc_requirements"] = {
          {"minimum", Value(data["pc_requirements"], "minimum", std::string())},
          {"recommended", Value(data["pc_requirements"], "recommended", std::string())},
      };
    }
    json dlc = json::array();
    for (const auto& id : Value(data, "dlc", json::array())) dlc.push_back(id);
    steam_info["dlc"] = dlc;
    json descriptors = json::array();
    if (data.contains("content_descriptors") && data["content_descriptors"].is_object()) {
      for (const auto& note : Value(data["content_descriptors"], "notes", json::array())) descriptors.push_back(note);
    }
    steam_info["content_descriptors"] = descriptors;
    if (data.contains("achievements")) {
      steam_info["achievements_total"] = Value(data["achievements"], "total", 0);
    }
    steam_info["controller_support"] = Value(data, "controller_support", std::string());
    json screenshots = json::array();
    for (const auto& shot : Value(data, "screenshots", json::array())) {
      const std::string url = Value(shot, "path_full", std::string());
      if (!url.empty()) screenshots.push_back(url);
    }
    steam_info["screenshots"] = screenshots;
    json movies = json::array();
    for (const auto& movie : Value(data, "movies", json::array())) {
      if (!movie.is_object()) continue;
      // Steam lists streams now; an mp4 only on older answers.
      std::string url = Value(movie, "hls_h264", std::string());
      if (url.empty() && movie.contains("mp4")) url = Value(movie["mp4"], "max", std::string());
      if (!url.empty()) movies.push_back(url);
    }
    steam_info["movies"] = movies;

    info["steam"] = steam_info;
  }
  AddStoreItemReviews(item, info);
  if (config.GetBool("tags.steam")) AddStoreItemTags(cache, appid, item, info);
  proton.join();
  info.update(proton_info);
  return !store.is_discarded();
}

void FetchSteamOwned(const config::Config& config, store::MetadataStore& cache, const std::string& appid,
                     const std::string& name, const std::string& game_id, std::int64_t griddb_id, json& info) {
  // One store request names the cover and carries the reviews and tags. The details don't touch
  // the art's keys, so they're fetched alongside it.
  const json item = SteamStoreItem(appid);
  json details;
  std::jthread details_thread = Spawn([&] { FetchSteamDetails(config, cache, appid, item, details); });

  // Steam's own cover/hero go into art_candidates too, as the first entry --
  // otherwise there was no way back to it once you picked a SteamGridDB
  // alternate. Negative id: a real SteamGridDB id is always positive.
  constexpr std::int64_t kSteamCdnCandidateId = -1;

  const std::string cover_url = SteamCoverUrl(appid, item);
  if (FetchArtworkInto(config, cover_url, game_id, "steam_cdn", "cover", info, kSteamCdnCandidateId)) {
    info["art_candidates"]["cover"] = json::array({{{"id", kSteamCdnCandidateId},
                                                     {"url", cover_url},
                                                     {"width", 600},
                                                     {"height", 900},
                                                     {"style", "steam"},
                                                     {"source", "steam_cdn"}}});
  }
  // Steam's own CDN serves this too, same appid, no key -- the wide banner
  // shown at the top of a game's store/library page, distinct from the
  // vertical library_600x900 cover above.
  const std::string hero_url =
      std::format("https://cdn.akamai.steamstatic.com/steam/apps/{}/library_hero.jpg", appid);
  if (FetchArtworkInto(config, hero_url, game_id, "steam_cdn", "hero", info, kSteamCdnCandidateId)) {
    info["art_candidates"]["hero"] = json::array({{{"id", kSteamCdnCandidateId},
                                                    {"url", hero_url},
                                                    {"width", 3840},
                                                    {"height", 1240},
                                                    {"style", "steam"},
                                                    {"source", "steam_cdn"}}});
  }
  // Steam's own art above is already the default; this only adds
  // SteamGridDB's candidates as alternates, appended after the Steam entry
  // already seeded above, so Steam's own image stays first. Never fails the
  // fetch -- a Steam-owned game already has its cover either way.
  if (const std::string api_key = config.GetString("steamgriddb.api_key"); !api_key.empty()) {
    FetchGriddbCandidates(config, api_key, name, game_id, griddb_id, info);
  }
  details_thread.join();
  info.update(details);
}

// Legendary's own catalog cache already has title metadata and store art
// URLs for every title it knows about, populated by `legendary list`, read
// here directly rather than hitting Epic's API again (Legendary already did
// that work). Never fails: a cold/missing cache entry (a title added before
// any `legendary list` refresh) falls back to SteamGridDB by name, same as
// any other non-Steam game.
void FetchEpicOwned(const config::Config& config, const std::string& app_name, const std::string& name,
                    const std::string& game_id, std::int64_t griddb_id, json& info) {
  if (const json meta = LegendaryMeta(app_name); !meta.is_null()) {
    AddEpicDetails(app_name, meta, info);

    // A tall cover and a wide hero, the same two slots Steam's CDN fills in
    // FetchSteamOwned above.
    const json key_images = Value(meta, "keyImages", json::array());

    // Steam CDN candidates use id -1 above; a real SteamGridDB id is always
    // positive, so any small negative id is safe as a fixed marker.
    constexpr std::int64_t kEpicCandidateId = -2;
    if (const std::string cover_url = FindKeyImage(key_images, kEpicCoverTypes); !cover_url.empty()) {
      if (FetchArtworkInto(config, cover_url, game_id, "epic", "cover", info, kEpicCandidateId)) {
        info["art_candidates"]["cover"] =
            json::array({{{"id", kEpicCandidateId}, {"url", cover_url}, {"source", "epic"}}});
      }
    }
    if (const std::string hero_url = FindKeyImage(key_images, kEpicHeroTypes); !hero_url.empty()) {
      if (FetchArtworkInto(config, hero_url, game_id, "epic", "hero", info, kEpicCandidateId)) {
        info["art_candidates"]["hero"] =
            json::array({{{"id", kEpicCandidateId}, {"url", hero_url}, {"source", "epic"}}});
      }
    }
  }

  // Epic's own art above is already the default; SteamGridDB only adds
  // alternates, same trailing call FetchSteamOwned makes. Never fails the
  // fetch either way: Epic-owned art is there regardless of a key.
  if (const std::string api_key = config.GetString("steamgriddb.api_key"); !api_key.empty()) {
    FetchGriddbCandidates(config, api_key, name, game_id, griddb_id, info);
  }
}

// Lutris caches its own per-game art on disk, keyed by slug: <lutris_data_dir>/coverart/<slug>.jpg,
// <lutris_data_dir>/banners/<slug>.jpg, and (under XDG_DATA_HOME directly,
// not the lutris subdir) icons/hicolor/128x128/apps/lutris_<slug>.png.
// Lutris's has_custom_* pga.db columns mean "user overrode Lutris's own
// art", not "art exists" -- so the files are probed directly rather than
// trusting those columns. `url` is a file:// URI: FetchArtworkInto's own
// curl download already handles that scheme with no changes needed.
// Returns whether any Lutris art was found.
bool FetchLutrisOwned(const config::Config& config, const std::string& slug, const std::string& game_id,
                      json& info) {
  if (slug.empty()) return false;
  bool found = false;

  // Steam CDN candidates use id -1, Epic -2 -- any small negative id is
  // safe as a fixed marker since a real SteamGridDB id is always positive.
  constexpr std::int64_t kLutrisCandidateId = -3;
  auto try_slot = [&](const fs::path& file, std::string_view slot) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return;
    const std::string url = "file://" + file.string();
    if (FetchArtworkInto(config, url, game_id, "lutris", slot, info, kLutrisCandidateId)) {
      info["art_candidates"][std::string(slot)] =
          json::array({{{"id", kLutrisCandidateId}, {"url", url}, {"source", "lutris"}}});
      found = true;
    }
  };

  if (const auto data_dir = lutris::FindLutrisDataDir(config)) {
    try_slot(*data_dir / "coverart" / (slug + ".jpg"), "cover");
    try_slot(*data_dir / "banners" / (slug + ".jpg"), "hero");
  }
  const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
  const fs::path data_home = (xdg_data_home && *xdg_data_home) ? fs::path(xdg_data_home) : paths::Home() / ".local" / "share";
  try_slot(data_home / "icons" / "hicolor" / "128x128" / "apps" / ("lutris_" + slug + ".png"), "icon");

  return found;
}

// With metadata.steam_by_name, a non-Steam game's ProtonDB tier from its closest Steam match, and
// the reviews and tags of a Steam game of exactly its name, since a wrong game's would mislead. A
// store's launcher isn't a game, so it gets none. Returns whether a match was found for the tier.
bool AddSteamByName(const config::Config& config, store::MetadataStore& cache, const model::Game& game,
                    SteamMatcher& steam_match, json& info) {
  if (!config.GetBool("metadata.steam_by_name") || game.source == "launcher") return false;
  const std::string& exact = steam_match().exact;
  json item;
  std::jthread lookup;
  if (!exact.empty()) lookup = Spawn([&] { item = SteamStoreItem(exact); });
  const std::string& best = steam_match().best;
  if (!best.empty()) FetchProtonDb(best, info);
  if (lookup.joinable()) lookup.join();
  AddStoreItemReviews(item, info);
  if (config.GetBool("tags.steam")) AddStoreItemTags(cache, exact, item, info);
  return !best.empty();
}

// The store info, reviews, tags and ProtonDB tier, without art. Sets details_fetched unless
// Steam's store didn't answer, so a rate-limited try is made again.
void FetchDetails(const config::Config& config, store::MetadataStore& cache, const model::Game& game,
                  SteamMatcher& steam_match, json& info) {
  bool answered = true;
  if (game.runner_ref.starts_with("steam:")) {
    const std::string appid = game.runner_ref.substr(std::string_view("steam:").size());
    answered = FetchSteamDetails(config, cache, appid, SteamStoreItem(appid), info);
  } else {
    if (game.source == "epic") {
      if (const json meta = LegendaryMeta(game.source_ref); !meta.is_null()) AddEpicDetails(game.source_ref, meta, info);
    }
    AddSteamByName(config, cache, game, steam_match, info);
  }
  if (answered) {
    info["details_fetched"] = model::NowSeconds();
    info["details_version"] = kDetailsVersion;
  }
}

Result<void> FetchNonSteam(const config::Config& config, store::MetadataStore& cache, const model::Game& game,
                           std::int64_t griddb_id, json& info) {
  const std::string& name = game.name;
  const std::string& game_id = game.id;
  // Independent of the SteamGridDB key below -- ProtonDB's own by-AppID
  // lookup needs no key, only a best-matched AppID, so this runs first and
  // can still leave something cached even when there's no key for cover art.
  SteamMatcher steam_match(config, game);
  const bool found_protondb = AddSteamByName(config, cache, game, steam_match, info);

  const std::string api_key = config.GetString("steamgriddb.api_key");
  if (!api_key.empty()) FetchGriddbCandidates(config, api_key, name, game_id, griddb_id, info);

  // No key, or SteamGridDB had nothing: Steam's own art, if Steam sells a
  // game of exactly this name.
  if (!info.contains("artwork") && config.GetBool("metadata.steam_art_by_name")) {
    if (const std::string appid = steam_match().exact; !appid.empty()) {
      FetchArtworkInto(config, SteamCoverUrl(appid), game_id, "steam_cdn", "cover", info);
      if (!info.contains("hero")) {
        FetchArtworkInto(config, std::format("https://cdn.akamai.steamstatic.com/steam/apps/{}/library_hero.jpg", appid),
                         game_id, "steam_cdn", "hero", info);
      }
    }
  }

  // An error rather than a silent skip when there's nothing to show and no
  // key: reporting success left the caller with a cache entry, a
  // game.metadata_ready event and no picture, which reads as "Mira looked
  // and there was nothing" rather than "Mira was never given the one thing
  // it needed". A found ProtonDB tier is still something, though.
  if (api_key.empty() && !info.contains("artwork") && !found_protondb) {
    return NoGriddbKey("no cover found for this game without a SteamGridDB API key");
  }
  return {};
}

}  // namespace

std::optional<bool> SteamSaysSoftware(const std::string& name) {
  const SteamMatch match = FindSteamAppIds(name);
  if (match.exact.empty()) return std::nullopt;
  const json items = SteamStoreItems({match.exact}, false);
  if (items.empty()) return std::nullopt;
  switch (Value(items[0], "type", -1)) {
    case 0: return false;  // game
    case 6: return true;   // software
    default: return std::nullopt;
  }
}

Result<json> SearchSteamGridDb(const config::Config& config, const std::string& name) {
  const std::string api_key = config.GetString("steamgriddb.api_key");
  if (api_key.empty()) return NoGriddbKey("searching SteamGridDB needs an API key");
  const json search = CurlJson({"curl", "-sSL", "-H", std::format("Authorization: Bearer {}", api_key),
                                std::format("https://www.steamgriddb.com/api/v2/search/autocomplete/{}",
                                           UrlEncode(name))});
  if (search.is_discarded() || !Value(search, "success", false)) {
    return Err("steamgriddb_error", "SteamGridDB search failed");
  }
  json matches = json::array();
  for (const json& item : Value(search, "data", json::array())) {
    json match = {{"id", Value(item, "id", std::int64_t{0})}, {"name", Value(item, "name", std::string())}};
    if (item.contains("release_date") && item["release_date"].is_number()) match["release_date"] = item["release_date"];
    matches.push_back(std::move(match));
  }
  return matches;
}

std::filesystem::path ArtworkDir(const config::Config& config, const std::string& game_id) {
  return config.File().parent_path() / "artwork" / game_id;
}

// Slots the user picked by hand carry over; FetchArtworkInto then leaves them be.
void CarryChosenSlots(const json& old, json& info) {
  for (const auto& [key, value] : old.items()) {
    if (value.is_object() && Value(value, "chosen", false)) info[key] = value;
  }
}

Result<void> Fetch(const config::Config& config, store::MetadataStore& cache, const model::Game& game) {
  json info = {{"fetched_at", model::NowSeconds()},
               {"details_fetched", model::NowSeconds()},
               {"details_version", kDetailsVersion}};
  CarryChosenSlots(cache.Read(game.id), info);
  const std::int64_t griddb_id = config::Resolver(config, game.overrides).GetInt("metadata.steamgriddb_id");

  // Checked before the steam: prefix below: an Epic game's runner_ref is
  // "proton:..."/"wine:..." (it's launched through Mira's own Wine/Proton
  // runners, not Legendary; see epic/Legendary.h), indistinguishable from
  // a Lutris or scanned Windows game by runner_ref alone.
  if (game.source == "epic") {
    info["source"] = "epic";
    FetchEpicOwned(config, game.source_ref, game.name, game.id, griddb_id, info);
  } else if (game.source == "lutris") {
    // Lutris's cached art first; the generic fetch still adds ProtonDB and
    // SteamGridDB alternates, and covers games with no Lutris art.
    info["source"] = "lutris";
    const bool found = config.GetBool("lutris.import_art") && FetchLutrisOwned(config, game.source_ref, game.id, info);
    if (Result<void> fetched = FetchNonSteam(config, cache, game, griddb_id, info); !fetched && !found) {
      return std::unexpected(fetched.error());
    }
  } else if (game.runner_ref.starts_with("steam:")) {
    info["source"] = "steam";
    FetchSteamOwned(config, cache, game.runner_ref.substr(std::string_view("steam:").size()),
                    game.name, game.id, griddb_id, info);
  } else {
    // GOG, itch and Amazon games have their store's own art in gamesdb;
    // SteamGridDB then only adds alternates, so it may fail.
    const bool found = FetchStoreCover(config, game, info);
    info["source"] = found ? game.source : "steamgriddb";
    if (found) {
      if (const std::string_view platform = GamesDbPlatform(game.source); !platform.empty()) {
        if (const std::string hero = GamesDbImageUrl(platform, game.source_ref, "horizontal_artwork"); !hero.empty()) {
          FetchArtworkInto(config, hero, game.id, "gog_gamesdb", "hero", info);
        }
      }
    }
    // Returned before anything is written: a failure here means nothing was
    // fetched, and a cache file would make the next attempt look answered.
    if (Result<void> fetched = FetchNonSteam(config, cache, game, griddb_id, info); !fetched && !found) {
      return std::unexpected(fetched.error());
    }
  }

  // A pick made while this fetch ran stays.
  const std::lock_guard lock(MetadataFileMutex());
  CarryChosenSlots(cache.Read(game.id), info);
  return cache.Write(game.id, info);
}

namespace {

Result<void> FetchTitleWith(const config::Config& config, store::MetadataStore& cache, const model::Game& game,
                            const json* steam_item) {
  const json cached = cache.Read(game.id);
  json info = {{"fetched_at", model::NowSeconds()}, {"source", game.source}};
  SteamMatcher steam_match(config, game);

  if (config.GetBool("metadata.title_details") && !DetailsFresh(config, cached)) {
    if (steam_item && Value(*steam_item, "success", 0) == 1) {
      const std::string appid = game.runner_ref.substr(std::string_view("steam:").size());
      AddStoreItemDetails(appid, *steam_item, info);
      if (config.GetBool("tags.steam")) AddStoreItemTags(cache, appid, *steam_item, info);
      FetchProtonDb(appid, info);
      info["details_fetched"] = model::NowSeconds();
      info["details_version"] = kDetailsVersion;
    } else {
      FetchDetails(config, cache, game, steam_match, info);
    }
  }

  const std::string api_key = config.GetString("steamgriddb.api_key");
  if (!cache.ArtFor(game.id, "cover")) {
    if (!FetchStoreCover(config, game, info, steam_item) && !api_key.empty()) {
      const std::string auth_header = std::format("Authorization: Bearer {}", api_key);
      if (const std::int64_t griddb_id = FindGriddbId(auth_header, game.name); griddb_id != 0) {
        FetchGriddbSlot(config, auth_header, griddb_id, game.id, "grids", "cover", info);
      }
    }
    // Same last resort as a tracked game's: Steam's art for the same name.
    if (!info.contains("artwork") && game.source != "steam" && config.GetBool("metadata.steam_art_by_name")) {
      if (const std::string& appid = steam_match().exact; !appid.empty()) {
        FetchArtworkInto(config, SteamCoverUrl(appid), game.id, "steam_cdn", "cover", info);
      }
    }
    if (info.contains("artwork")) RefitSlot(ArtworkDir(config, game.id), info["artwork"], ArtBox(config, "cover", true));
  }

  // Merged into any fuller record rather than replacing it.
  {
    const std::lock_guard lock(MetadataFileMutex());
    json merged = cache.Read(game.id);
    for (const auto& [key, value] : info.items()) {
      if (!(merged.contains(key) && merged[key].is_object() && Value(merged[key], "chosen", false))) merged[key] = value;
    }
    if (auto written = cache.Write(game.id, merged); !written) return written;
  }
  if (!cache.ArtFor(game.id, "cover")) {
    if (api_key.empty()) return NoGriddbKey("no cover found for this game without a SteamGridDB API key");
    return Err("no_artwork", "no cover found for \"" + game.name + "\"");
  }
  return {};
}

}  // namespace

Result<void> FetchTitle(const config::Config& config, store::MetadataStore& cache, const model::Game& game) {
  return FetchTitleWith(config, cache, game, nullptr);
}

std::vector<Result<void>> FetchSteamTitles(const config::Config& config, store::MetadataStore& cache,
                                           const std::vector<model::Game>& titles) {
  std::vector<std::string> appids;
  for (const model::Game& title : titles) appids.push_back(title.runner_ref.substr(std::string_view("steam:").size()));
  std::map<std::string, json> items;
  for (const json& item : SteamStoreItems(appids, config.GetBool("metadata.title_details"))) {
    items[std::to_string(Value(item, "appid", std::int64_t{0}))] = item;
  }

  std::vector<Result<void>> results(titles.size(), Err("fetch_failed", "the fetch stopped unexpectedly"));
  std::atomic<std::size_t> next{0};
  const auto work = [&] {
    for (std::size_t i; (i = next++) < titles.size();) {
      const auto item = items.find(appids[i]);
      results[i] = FetchTitleWith(config, cache, titles[i], item == items.end() ? nullptr : &item->second);
    }
  };
  {
    // A few at a time: each is a ProtonDB lookup and a cover download.
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i) workers.push_back(Spawn(work));
  }
  return results;
}

Result<void> RefreshDetails(const config::Config& config, store::MetadataStore& cache, const model::Game& game) {
  json info = json::object();
  SteamMatcher steam_match(config, game);
  FetchDetails(config, cache, game, steam_match, info);
  if (!info.contains("details_fetched")) return Err("details_unavailable", "the store didn't answer");
  const std::lock_guard lock(MetadataFileMutex());
  json merged = cache.Read(game.id);
  if (merged.empty()) return Err("metadata_not_found", "no metadata cached for this game yet");
  merged.update(info);
  return cache.Write(game.id, merged);
}

bool TitleNeedsFetch(const config::Config& config, const store::MetadataStore& cache, const std::string& id) {
  if (!cache.ArtVersions(id).contains("cover")) return true;
  return config.GetBool("metadata.title_details") && !DetailsFresh(config, cache.Read(id));
}

bool DetailsFresh(const config::Config& config, const json& info) {
  const std::int64_t fetched = Value(info, "details_fetched", std::int64_t{0});
  if (fetched == 0 || Value(info, "details_version", 1) < kDetailsVersion) return false;
  const std::int64_t days = config.GetInt("metadata.refresh_days");
  return days == 0 || model::NowSeconds() - fetched < days * 86400;
}

void FitCachedArt(const config::Config& config, store::MetadataStore& cache,
                  const std::function<bool(const std::string&)>& is_title) {
  const std::vector<std::string> ids = cache.PendingFits();
  if (ids.empty()) return;
  log::Info("shrinking the art of {} cached games", ids.size());
  constexpr std::array<std::pair<const char*, const char*>, 4> kFitted = {
      {{"cover", "artwork"}, {"hero", "hero"}, {"logo", "logo"}, {"icon", "icon"}}};
  for (const std::string& id : ids) {
    if (ThisTaskStop().stop_requested()) return;  // the rest resume next start
    const fs::path dir = ArtworkDir(config, id);
    const json before = cache.Read(id);
    const bool title = is_title(id);

    // Fitted beside the originals without the lock: decoding takes a while.
    struct Fit {
      const char* key;
      std::string old_name;
      fs::path temp, dest;
    };
    std::vector<Fit> fits;
    for (const auto& [slot, key] : kFitted) {
      const std::string name = Value(Value(before, key, json::object()), "file", std::string());
      if (name.empty() || name.find('/') != std::string::npos) continue;
      const auto fitted = FitImage(dir / name, ArtBox(config, slot, title));
      if (!fitted) continue;
      fs::path dest = dir / name;
      dest.replace_extension(fitted->ext);
      const fs::path temp = dest.string() + ".fitting";
      if (WriteFileAtomic(temp, fitted->bytes, "artwork_write_failed")) fits.push_back({key, name, temp, dest});
    }

    std::vector<fs::path> stale;  // removed once the record no longer names them
    bool saved = true;
    {
      const std::lock_guard lock(MetadataFileMutex());
      json info = cache.Read(id);
      std::error_code ec;
      for (const Fit& fit : fits) {
        // A fetch that replaced the slot meanwhile wins.
        if (Value(Value(info, fit.key, json::object()), "file", std::string()) != fit.old_name) {
          fs::remove(fit.temp, ec);
          continue;
        }
        fs::rename(fit.temp, fit.dest, ec);
        if (ec) {
          fs::remove(fit.temp, ec);
          continue;
        }
        info[fit.key]["file"] = fit.dest.filename().string();
        info[fit.key]["content_type"] = ContentTypeFor(fit.dest);
        if (fit.dest != dir / fit.old_name) stale.push_back(dir / fit.old_name);
      }
      for (const char* key : {"capsule", "header"}) {
        if (const std::string name = Value(Value(info, key, json::object()), "file", std::string());
            !name.empty() && name.find('/') == std::string::npos) {
          stale.push_back(dir / name);
        }
        info.erase(key);
      }
      if (!info.empty()) {
        if (auto written = cache.Write(id, info); !written) {
          log::Warn("couldn't save {}'s fitted art: {}", id, written.error().message);
          saved = false;
        }
      }
    }
    if (!saved) continue;  // tried again next start; the originals are still there
    std::error_code ec;
    for (const fs::path& file : stale) fs::remove(file, ec);
    cache.FitDone(id);
  }
}

void PruneOrphanArt(const config::Config& config, const store::MetadataStore& cache) {
  const fs::path root = config.File().parent_path() / "artwork";
  const auto cutoff = fs::file_time_type::clock::now() - std::chrono::days(1);
  std::error_code ec;
  int pruned = 0;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    const std::string id = entry.path().filename().string();
    std::error_code entry_ec;
    if (!entry.is_directory(entry_ec) || cache.Has(id) || fs::last_write_time(entry.path(), entry_ec) > cutoff) continue;
    fs::remove_all(entry.path(), entry_ec);
    ++pruned;
  }
  if (pruned > 0) log::Info("removed the art of {} games no longer cached", pruned);
}

// One pick or upload at a time, so two for one slot can't land their files and records in a
// different order.
std::mutex& PickMutex() {
  static std::mutex picking;
  return picking;
}

Result<void> SelectArtwork(const config::Config& config, store::MetadataStore& cache, const std::string& game_id,
                           const std::string& slot, std::int64_t candidate_id) {
  // The download itself runs without MetadataFileMutex, which every other game's fetch also needs.
  const std::lock_guard pick_lock(PickMutex());
  const auto read_info = [&]() -> Result<json> {
    if (!cache.Has(game_id)) return Err("metadata_not_found", "no metadata cached for this game yet");
    return cache.Read(game_id);
  };
  std::unique_lock lock(MetadataFileMutex());
  Result<json> read = read_info();
  if (!read) return std::unexpected(read.error());
  const json info = std::move(*read);

  if (!info.contains("art_candidates") || !info["art_candidates"].contains(slot)) {
    return Err("no_candidates", "no candidate list cached for this slot");
  }
  std::string url;
  // Cache entries written before candidates carried their own source (either
  // this field, pre-dating it entirely) default to "steamgriddb" -- every
  // candidate was one before the Steam CDN entry existed.
  std::string source = "steamgriddb";
  for (const auto& candidate : info["art_candidates"][slot]) {
    if (Value(candidate, "id", std::int64_t{-1}) == candidate_id) {
      url = Value(candidate, "url", std::string());
      source = Value(candidate, "source", std::string("steamgriddb"));
      break;
    }
  }
  if (url.empty()) return Err("candidate_not_found", "no such candidate id for this slot");

  // Looked up by id against the list this same code already fetched and
  // cached, rather than accepting a caller-supplied URL directly -- so the
  // daemon never ends up fetching an arbitrary URL on the API's behalf. No
  // credentials on the download itself -- see FetchArtworkInto.
  lock.unlock();
  const std::string key = slot == "cover" ? "artwork" : slot;
  json downloaded = json::object();  // without the slot's "chosen", which would skip the download
  if (!FetchArtworkInto(config, url, game_id, source, slot, downloaded, candidate_id,
                        /*picked=*/true)) {
    return Err("download_failed", "couldn't download the selected image", kConnectionHint);
  }

  // Read again: a fetch may have rewritten the file during the download.
  lock.lock();
  Result<json> current = read_info();
  if (!current) return std::unexpected(current.error());
  (*current)[key] = std::move(downloaded[key]);
  (*current)[key]["chosen"] = true;  // a refresh (Fetch) keeps it
  return cache.Write(game_id, *current);
}

Result<void> UploadArtwork(const config::Config& config, store::MetadataStore& cache, const std::string& game_id,
                           const std::string& slot, std::string_view bytes) {
  if (slot != "cover" && slot != "hero" && slot != "logo" && slot != "icon") {
    return Err("invalid_type", "unknown art slot");
  }
  const bool png = bytes.starts_with("\x89PNG");
  if (!png && !bytes.starts_with("\xFF\xD8\xFF")) return Err("unsupported_image", "only PNG and JPEG images can be used");
  const std::lock_guard pick_lock(PickMutex());

  const fs::path dir = ArtworkDir(config, game_id);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return Err("artwork_write_failed", "couldn't create the game's artwork folder: " + ec.message());
  const std::string ext = png ? ".png" : ".jpg";
  const fs::path part = dir / std::format("{}.upload{}.part", slot, ext);
  if (auto written = WriteFileAtomic(part, bytes, "artwork_write_failed"); !written) return std::unexpected(written.error());
  fs::path dest = dir / std::format("{}.chosen{}", slot, ext);
  if (!IsImage(part) || !CommitArt(part, dest, ArtBox(config, slot))) {
    fs::remove(part, ec);
    return Err("unsupported_image", "couldn't read that image");
  }

  const std::lock_guard lock(MetadataFileMutex());
  json info = cache.Read(game_id);
  info[slot == "cover" ? "artwork" : slot] = {{"file", dest.filename().string()},
                                              {"content_type", ContentTypeFor(dest)},
                                              {"source", "upload"},
                                              {"chosen", true}};
  return cache.Write(game_id, info);
}

Result<json> FetchCandidatePage(const config::Config& config, store::MetadataStore& cache, const std::string& game_id,
                                const std::string& slot, int page) {
  const std::string_view endpoint = slot == "cover"  ? "grids"
                                    : slot == "hero" ? "heroes"
                                    : slot == "logo" ? "logos"
                                    : slot == "icon" ? "icons"
                                                     : "";
  if (endpoint.empty()) return Err("invalid_type", "unknown art slot");
  const std::string api_key = config.GetString("steamgriddb.api_key");
  if (api_key.empty()) return NoGriddbKey("browsing SteamGridDB's art needs an API key");

  const std::int64_t griddb_id = Value(cache.Read(game_id), "steamgriddb_id", std::int64_t{0});
  if (griddb_id == 0) return Err("no_steamgriddb_match", "this game has no SteamGridDB match yet");

  const json response =
      FetchGriddbPage(config, std::format("Authorization: Bearer {}", api_key), endpoint, griddb_id, page);
  if (response.is_discarded() || !Value(response, "success", false)) {
    return Err("steamgriddb_unreachable", "couldn't reach SteamGridDB",
               "Check the internet connection. SteamGridDB also limits requests, so waiting a minute can help.");
  }
  json candidates = json::array();
  for (const auto& item : Value(response, "data", json::array())) {
    json candidate = GriddbCandidate(item);
    if (!candidate.is_null()) candidates.push_back(std::move(candidate));
  }

  // Added to the cached list, so selecting one and fetching its preview can
  // look it up by id like any other.
  {
    const std::lock_guard lock(MetadataFileMutex());
    if (cache.Has(game_id)) {
      json info = cache.Read(game_id);
      json& cached = info["art_candidates"][slot];
      if (!cached.is_array()) cached = json::array();
      for (const json& candidate : candidates) {
        const std::int64_t id = candidate["id"].get<std::int64_t>();
        const bool known = std::ranges::any_of(
            cached, [id](const json& entry) { return Value(entry, "id", std::int64_t{0}) == id; });
        if (!known) cached.push_back(candidate);
      }
      [[maybe_unused]] auto written = cache.Write(game_id, info);
    }
  }
  return json{{"page", page}, {"total", Value(response, "total", 0)}, {"candidates", candidates}};
}

namespace {

// The slot goes into a file name, so only a plain word is accepted.
bool IsSlotName(const std::string& slot) {
  return !slot.empty() && std::ranges::all_of(slot, [](unsigned char c) { return std::islower(c) != 0; });
}

fs::path ThumbCacheDir(const config::Config& config) { return config.File().parent_path() / "cache" / "thumbs"; }

fs::path ThumbPath(const config::Config& config, const std::string& game_id, const std::string& slot,
                   std::int64_t candidate_id) {
  return ThumbCacheDir(config) / game_id / std::format("{}_{}", slot, candidate_id);
}

}  // namespace

void ClearCandidateThumbs(const config::Config& config) {
  std::error_code ec;
  fs::remove_all(ThumbCacheDir(config), ec);
}

void ClearCandidateThumbs(const config::Config& config, const std::string& game_id) {
  std::error_code ec;
  fs::remove_all(ThumbCacheDir(config) / game_id, ec);
}

std::filesystem::path CandidateThumbFile(const config::Config& config, const std::string& game_id,
                                         const std::string& slot, std::int64_t candidate_id) {
  if (!IsSlotName(slot)) return {};
  const fs::path file = ThumbPath(config, game_id, slot, candidate_id);
  std::error_code ec;
  return fs::file_size(file, ec) > 0 && !ec ? file : fs::path();
}

Result<ThumbBatch> FetchCandidateThumbs(const config::Config& config, const store::MetadataStore& cache,
                                        const std::string& game_id, const std::string& slot,
                                        const std::vector<std::int64_t>& candidate_ids) {
  if (!IsSlotName(slot)) return Err("invalid_type", "unknown art slot");
  if (!cache.Has(game_id)) return Err("metadata_not_found", "no metadata cached for this game yet");
  const json info = cache.Read(game_id);
  const json candidates = info.contains("art_candidates") && info["art_candidates"].contains(slot)
                              ? info["art_candidates"][slot]
                              : json::array();

  const fs::path dir = ThumbCacheDir(config) / game_id;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return Err("artwork_dir_failed", ec.message());

  // Per batch, so two batches naming the same candidate never share a file.
  static std::atomic<unsigned> next_batch{0};
  const std::string suffix = std::format(".part{}", next_batch++);

  ThumbBatch batch;
  std::map<std::string, std::int64_t> pending;  // part file -> candidate id
  // One curl for the whole batch, fetching in parallel. -w reports each
  // transfer's own exit code, since the process's only says whether any failed.
  Command command;
  command.argv = {"curl", "-sSL", "-f", "--proto", "=file,https,http", "--max-time", std::string(kMaxTime), "--parallel", "--parallel-max", "8",
                  "-w", "%{exitcode} %{filename_effective}\n"};
  std::set<std::int64_t> seen;
  for (const std::int64_t id : candidate_ids) {
    if (!seen.insert(id).second) continue;
    if (!CandidateThumbFile(config, game_id, slot, id).empty()) {
      batch.ready.push_back(id);
      continue;
    }
    std::string url;
    for (const auto& candidate : candidates) {
      if (Value(candidate, "id", std::int64_t{0}) != id) continue;
      url = Value(candidate, "thumb", std::string());
      if (url.empty()) url = Value(candidate, "url", std::string());
      break;
    }
    if (url.empty()) {
      batch.failed.push_back(id);
      continue;
    }
    const std::string part = ThumbPath(config, game_id, slot, id).string() + suffix;
    pending[part] = id;
    command.argv.insert(command.argv.end(), {"-o", part, "--url", url});
  }
  if (pending.empty()) return batch;

  std::map<std::string, int> exit_codes;
  if (const Result<runner::ExecResult> ran = runner::RunAndWait(command); ran) {
    std::istringstream lines(ran->output);
    for (std::string line; std::getline(lines, line);) {
      const std::size_t space = line.find(' ');
      if (space == std::string::npos) continue;
      const std::string file = line.substr(space + 1);
      if (pending.contains(file)) exit_codes[file] = std::atoi(line.substr(0, space).c_str());
    }
  }
  for (const auto& [part, id] : pending) {
    const auto code = exit_codes.find(part);
    const bool ok = code != exit_codes.end() && code->second == 0 && fs::file_size(part, ec) > 0 && !ec;
    if (ok) fs::rename(part, ThumbPath(config, game_id, slot, id), ec);
    if (!ok || ec) {
      fs::remove(part, ec);
      batch.failed.push_back(id);
    } else {
      batch.ready.push_back(id);
    }
  }
  return batch;
}

std::unordered_map<std::string, std::optional<std::vector<std::string>>> StoredSteamTags(
    const store::MetadataStore& cache) {
  std::unordered_map<std::string, std::optional<std::vector<std::string>>> out;
  for (auto& [id, record] : cache.Field("steam_tags")) {
    std::optional<std::vector<std::string>>& tags = out[id];
    if (!record.is_object()) continue;
    tags.emplace();
    for (const auto& tag : Value(record, "tags", json::array())) {
      if (tag.is_string()) tags->push_back(tag.get<std::string>());
    }
  }
  return out;
}

bool WantsSteamTags(const config::Config& config, const model::Game& game) {
  if (!config.GetBool("tags.steam") || game.source == "launcher") return false;
  return game.runner_ref.starts_with("steam:") || config.GetBool("metadata.steam_by_name");
}

int FetchSteamTags(const config::Config& config, store::MetadataStore& cache, std::span<const model::Game> games) {
  // Each game's appid: Steam's own, else its Steam match by exact name (empty when none).
  std::map<std::string, std::string> appid_of;
  for (const model::Game& game : games) {
    if (!WantsSteamTags(config, game) || !cache.Has(game.id)) continue;  // its own fetch brings them
    appid_of[game.id] = game.runner_ref.starts_with("steam:")
                            ? game.runner_ref.substr(std::string_view("steam:").size())
                            : SteamMatcher(config, game)().exact;
  }
  std::vector<std::string> appids;
  for (const auto& [id, appid] : appid_of) {
    if (!appid.empty() && !std::ranges::contains(appids, appid)) appids.push_back(appid);
  }
  std::map<std::string, json> items;
  constexpr std::size_t kBatch = 50;
  for (std::size_t start = 0; start < appids.size(); start += kBatch) {
    const std::vector<std::string> batch(appids.begin() + start, appids.begin() + std::min(appids.size(), start + kBatch));
    for (const json& item : SteamStoreItems(batch, true)) {
      items[std::to_string(Value(item, "appid", std::int64_t{0}))] = item;
    }
  }
  std::map<std::string, json> found;
  for (const auto& [id, appid] : appid_of) {
    json info;
    AddStoreItemTags(cache, appid, items[appid], info);
    if (info.contains("steam_tags")) found[id] = std::move(info["steam_tags"]);
  }
  int fetched = 0;
  const std::lock_guard lock(MetadataFileMutex());
  for (auto& [id, tags] : found) {
    json info = cache.Read(id);
    info["steam_tags"] = std::move(tags);
    if (cache.Write(id, info)) ++fetched;
  }
  return fetched;
}

}  // namespace mira::metadata
