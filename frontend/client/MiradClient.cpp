#include "MiradClient.h"

#include <json.hpp>

#include <cctype>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "Async.h"
#include "Jobs.h"
#include "JsonMapping.h"
#include "Transport.h"

namespace mira_gui {
namespace {

using nlohmann::json;

QImage DecodeImage(const std::string& bytes) {
  QImage image;
  image.loadFromData(reinterpret_cast<const uchar*>(bytes.data()), static_cast<int>(bytes.size()));
  return image;
}

// Percent-encodes everything outside RFC 3986's unreserved set.
std::string QueryEncode(const std::string& text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

// Each of these is the blocking half of one endpoint, run on a worker thread
// by async::Run below. They are written as "ask transport, shape the reply"
// and nothing else: no socket, no timeouts, no error unwrapping.

HealthStatus GetHealthSync() {
  HealthStatus status;
  const transport::Reply reply = transport::Get("/v1/health");
  status.reachable = reply.ok;
  status.detail = reply.ok ? reply.body.value("status", std::string("ok")) : reply.error.message;
  if (reply.ok && reply.body.is_object() && reply.body.contains("api") && reply.body["api"].is_number_integer()) {
    status.api = reply.body["api"].get<int>();
  }
  return status;
}

GamesResult GetGamesSync(const std::string& status_filter, const std::string& tag_filter, bool include_hidden) {
  GamesResult result;
  std::string path = "/v1/games";
  std::string separator = "?";
  if (!status_filter.empty()) {
    path += separator + "status=" + status_filter;
    separator = "&";
  }
  if (!tag_filter.empty()) {
    path += separator + "tag=" + tag_filter;
    separator = "&";
  }
  if (include_hidden) path += separator + "include_hidden=true";
  const transport::Reply reply = transport::Get(path);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/games");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) result.games.push_back(mapping::ToGameSummary(entry));
  return result;
}

DeleteResult DeleteGameSync(const std::string& id, bool delete_files, bool delete_prefix,
                            bool delete_metadata) {
  // Every flag is opt-in server-side too: the bare DELETE never touches
  // disk, so an omitted param and "false" mean the same thing.
  std::string path = "/v1/games/" + id;
  std::string separator = "?";
  if (delete_files) {
    path += separator + "delete_files=true";
    separator = "&";
  }
  if (delete_prefix) {
    path += separator + "delete_prefix=true";
    separator = "&";
  }
  if (delete_metadata) path += separator + "delete_metadata=true";

  const transport::Reply reply = transport::Delete(path);
  return {reply.ok, reply.error};
}

LaunchResult LaunchGameSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + id + "/launch");
  // mirad answers `tracked` directly (docs/api.md): whether game.state
  // events are coming for this launch.
  const bool tracked = !reply.body.is_object() ||
                       reply.body.value("tracked",
                                        reply.body.value("status", std::string()) !=
                                            "launched_via_steam");
  return {reply.ok, reply.error, tracked};
}

StopResult StopGameSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + id + "/stop");
  return {reply.ok, reply.error};
}

// Starts a job (docs/api.md#jobs) and hands its outcome to `callback` as an
// R, with `fill` reading the job's result. `start` sends the request with
// `query` ("?job=<token>") appended, so the waiter is listening before the 202.
template <typename R>
void RunJob(QObject* context, const std::string& kind, std::function<transport::Reply(const std::string&)> start,
            std::function<void(R&, const json&)> fill, std::function<void(R)> callback) {
  const std::string token = jobs::NewToken(kind);
  jobs::Await(context, token, [fill, callback](jobs::Outcome outcome) {
    R result;
    if (!outcome.ok) {
      result.error = outcome.error;
    } else {
      try {
        fill(result, outcome.result);
        result.ok = true;
      } catch (const std::exception& e) {
        result.error = e.what();
      }
    }
    callback(std::move(result));
  });
  async::Run(context, [start, token] { return start("?job=" + token); },
             std::function<void(transport::Reply)>([token, callback](transport::Reply reply) {
               if (reply.ok) {
                 jobs::Started(token);
                 return;
               }
               // Refused before it started (a bad body, an unknown source, mirad down).
               if (!jobs::Forget(token)) return;
               R result;
               result.error = reply.error;
               callback(std::move(result));
             }));
}

void FillScan(ScanResult& result, const json& body) {
  result.added = body.value("added", 0);
  result.missing = body.value("missing", 0);
  result.restored = body.value("restored", 0);
}

GameDetailResult GetGameSync(const std::string& id) {
  GameDetailResult result;
  const transport::Reply reply = transport::Get("/v1/games/" + id);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/games/" + id);
    return result;
  }

  result.ok = true;
  result.game = mapping::ToGameDetail(reply.body);
  return result;
}

PatchGameResult PatchGameSync(const std::string& id, const GamePatch& patch) {
  PatchGameResult result;
  json body = json::object();
  if (patch.name) body["name"] = *patch.name;
  if (patch.exe_path) body["exe_path"] = *patch.exe_path;
  if (patch.args) body["args"] = *patch.args;
  if (patch.working_dir) body["working_dir"] = *patch.working_dir;
  if (patch.runner_ref) body["runner_ref"] = *patch.runner_ref;
  if (patch.data_dir) body["data_dir"] = *patch.data_dir;
  if (patch.tags) body["tags"] = *patch.tags;

  const auto parse_object = [&](const std::string& text, const char* field,
                                const char* message) -> bool {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      result.error = message;
      return false;
    }
    body[field] = parsed;
    return true;
  };
  if (patch.runner_config_json &&
      !parse_object(*patch.runner_config_json, "runner_config",
                    "Runner config must be a JSON object, e.g. {}")) {
    return result;
  }
  if (patch.env_json && !parse_object(*patch.env_json, "env",
                                      "Environment must be a JSON object of strings, e.g. {}")) {
    return result;
  }

  const transport::Reply reply = transport::Patch("/v1/games/" + id, body);
  return {reply.ok, reply.error};
}

ConfigSchemaResult GetConfigSchemaSync() {
  ConfigSchemaResult result;
  const transport::Reply reply = transport::Get("/v1/config/schema");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/config/schema");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    ConfigSchemaEntry e;
    e.key = entry.value("key", std::string());
    e.type = entry.value("type", std::string());
    e.label = entry.value("label", std::string());
    e.doc = entry.value("doc", std::string());
    e.game_doc = entry.value("game_doc", std::string());
    e.category = entry.value("category", std::string());
    e.group = entry.value("group", 0);
    e.group_label = entry.value("group_label", std::string());
    e.group_collapsed = entry.value("group_collapsed", false);
    e.group_resettable = entry.value("group_resettable", false);
    e.source = entry.value("source", std::string());
    e.path = entry.value("path", std::string());
    const std::string scope = entry.value("scope", std::string());
    e.per_game = scope == "per_game" || scope == "game_only";
    e.game_only = scope == "game_only";
    e.is_secret = entry.value("is_secret", false);
    e.is_runner_ref = entry.value("is_runner_ref", false);
    e.link = entry.value("link", std::string());
    e.keywords = entry.value("keywords", std::string());
    if (entry.contains("default")) e.default_display = mapping::ToDisplayString(entry["default"]);
    if (entry.contains("one_of") && entry["one_of"].is_array()) {
      for (const json& option : entry["one_of"]) {
        if (option.is_string()) e.one_of.push_back(option.get<std::string>());
      }
    }
    if (entry.contains("minimum") && entry["minimum"].is_number()) {
      e.minimum = entry["minimum"].get<double>();
    }
    if (entry.contains("maximum") && entry["maximum"].is_number()) {
      e.maximum = entry["maximum"].get<double>();
    }
    result.entries.push_back(std::move(e));
  }
  return result;
}

ConfigResult GetConfigSync() {
  ConfigResult result;
  const transport::Reply reply = transport::Get("/v1/config");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/config");
    return result;
  }

  result.ok = true;
  mapping::FlattenConfig(reply.body, "", result.values);
  return result;
}

PatchConfigResult PatchConfigSync(const std::vector<ConfigEdit>& edits) {
  json body = json::object();
  for (const ConfigEdit& edit : edits) {
    mapping::AssignDottedKey(body, edit.key, mapping::TypedValueFromText(edit.type, edit.value));
  }
  const transport::Reply reply = transport::Patch("/v1/config", body);
  return {reply.ok, reply.error};
}

PatchConfigResult ResetConfigKeySync(const std::string& key) {
  const transport::Reply reply = transport::Post("/v1/config/reset?key=" + key);
  return {reply.ok, reply.error};
}

RunnersResult GetRunnersSync() {
  RunnersResult result;
  const transport::Reply reply = transport::Get("/v1/runners");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    RunnerInfo runner;
    runner.kind = entry.value("kind", std::string());
    runner.name = entry.value("name", std::string());
    runner.version = entry.value("version", std::string());
    runner.reference = entry.value("reference", std::string());
    runner.path = entry.value("path", std::string());
    runner.label = entry.value("label", runner.name);
    runner.source = entry.value("source", std::string());
    runner.removable = entry.value("removable", false);
    result.runners.push_back(std::move(runner));
  }
  return result;
}

GameConfigResult GetGameConfigSync(const std::string& id) {
  GameConfigResult result;
  const transport::Reply reply = transport::Get("/v1/games/" + id + "/config");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/games/" + id + "/config");
    return result;
  }

  result.ok = true;
  // Schema::Entries() order.
  for (const auto& [key, entry] : reply.body.items()) {
    GameConfigEntry e;
    e.key = key;
    e.value_display = mapping::ToDisplayString(entry.value("value", json()));
    e.layer = entry.value("layer", std::string());
    e.overridable = entry.value("overridable", false);
    result.entries.push_back(std::move(e));
  }
  return result;
}

FrontendPrefsResult GetFrontendPrefsSync() {
  FrontendPrefsResult result;
  const transport::Reply reply = transport::Get("/v1/config");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }

  result.ok = true;
  // Frontend falls back to defaults rather than refusing to start on absense
  // or wrong kind after a hand-edit.
  const json table = reply.body.value("frontend", json::object());
  if (!table.is_object()) return result;

  const auto read_int = [&table](const char* key, std::optional<int>& out) {
    if (table.contains(key) && table[key].is_number_integer()) out = table[key].get<int>();
  };
  read_int("window_width", result.prefs.window_width);
  read_int("window_height", result.prefs.window_height);
  read_int("tile_width", result.prefs.tile_width);
  read_int("sidebar_width", result.prefs.sidebar_width);
  const auto read_string = [&table](const char* key, std::optional<std::string>& out) {
    if (table.contains(key) && table[key].is_string()) out = table[key].get<std::string>();
  };
  const auto read_bool = [&table](const char* key, std::optional<bool>& out) {
    if (table.contains(key) && table[key].is_boolean()) out = table[key].get<bool>();
  };
  read_bool("window_maximized", result.prefs.window_maximized);
  read_string("library_filter", result.prefs.library_filter);
  read_string("sort_by", result.prefs.sort_by);
  read_bool("sort_descending", result.prefs.sort_descending);
  read_bool("scan_on_startup", result.prefs.scan_on_startup);
  read_string("theme", result.prefs.theme);
  read_bool("drag_select", result.prefs.drag_select);
  read_int("tile_spacing", result.prefs.tile_spacing);
  read_int("grid_margin", result.prefs.grid_margin);
  read_int("tile_radius", result.prefs.tile_radius);
  read_int("panel_radius", result.prefs.panel_radius);
  read_int("control_radius", result.prefs.control_radius);
  read_int("sidebar_recent_count", result.prefs.sidebar_recent_count);
  read_bool("sidebar_source_counts", result.prefs.sidebar_source_counts);
  read_bool("sidebar_source_icons", result.prefs.sidebar_source_icons);
  read_string("sidebar_pinned_style", result.prefs.sidebar_pinned_style);
  read_string("sidebar_recent_style", result.prefs.sidebar_recent_style);
  read_bool("sidebar_recent_when", result.prefs.sidebar_recent_when);
  read_bool("library_filter_tabs", result.prefs.library_filter_tabs);
  read_bool("library_continue_row", result.prefs.library_continue_row);
  read_int("library_continue_count", result.prefs.library_continue_count);
  read_bool("tile_status", result.prefs.tile_status);
  read_bool("tile_source_mark", result.prefs.tile_source_mark);
  read_bool("tile_pin_badge", result.prefs.tile_pin_badge);
  read_bool("source_page_tabs", result.prefs.source_page_tabs);
  read_bool("tile_size_synced", result.prefs.tile_size_synced);
  if (table.contains("source_tile_widths") && table["source_tile_widths"].is_object()) {
    std::map<std::string, int> widths;
    for (const auto& [id, width] : table["source_tile_widths"].items()) {
      if (width.is_number_integer()) widths[id] = width.get<int>();
    }
    result.prefs.source_tile_widths = std::move(widths);
  }
  if (table.contains("shortcuts") && table["shortcuts"].is_object()) {
    std::map<std::string, std::string> overrides;
    for (const auto& [id, keys] : table["shortcuts"].items()) {
      if (keys.is_string()) overrides[id] = keys.get<std::string>();
    }
    result.prefs.shortcut_overrides = std::move(overrides);
  }
  const auto read_strings = [&table](const char* key, std::optional<std::vector<std::string>>& out) {
    if (!table.contains(key) || !table[key].is_array()) return;
    std::vector<std::string> values;
    for (const json& value : table[key]) {
      if (value.is_string()) values.push_back(value.get<std::string>());
    }
    out = std::move(values);
  };
  read_strings("hidden_sources", result.prefs.hidden_sources);
  read_strings("source_order", result.prefs.source_order);

  if (table.contains("source_imported_at") && table["source_imported_at"].is_object()) {
    std::map<std::string, std::int64_t> imported;
    for (const auto& [id, at] : table["source_imported_at"].items()) {
      if (at.is_number_integer()) imported[id] = at.get<std::int64_t>();
    }
    result.prefs.source_imported_at = std::move(imported);
  }
  return result;
}

PatchConfigResult SaveFrontendPrefsSync(const FrontendPrefs& prefs) {
  json table = json::object();
  if (prefs.window_width) table["window_width"] = *prefs.window_width;
  if (prefs.window_height) table["window_height"] = *prefs.window_height;
  if (prefs.window_maximized) table["window_maximized"] = *prefs.window_maximized;
  if (prefs.tile_width) table["tile_width"] = *prefs.tile_width;
  if (prefs.sidebar_width) table["sidebar_width"] = *prefs.sidebar_width;
  if (prefs.library_filter) table["library_filter"] = *prefs.library_filter;
  if (prefs.sort_by) table["sort_by"] = *prefs.sort_by;
  if (prefs.sort_descending) table["sort_descending"] = *prefs.sort_descending;
  if (prefs.scan_on_startup) table["scan_on_startup"] = *prefs.scan_on_startup;
  if (prefs.theme) table["theme"] = *prefs.theme;
  if (prefs.drag_select) table["drag_select"] = *prefs.drag_select;
  if (prefs.tile_spacing) table["tile_spacing"] = *prefs.tile_spacing;
  if (prefs.grid_margin) table["grid_margin"] = *prefs.grid_margin;
  if (prefs.tile_radius) table["tile_radius"] = *prefs.tile_radius;
  if (prefs.panel_radius) table["panel_radius"] = *prefs.panel_radius;
  if (prefs.control_radius) table["control_radius"] = *prefs.control_radius;
  if (prefs.shortcut_overrides) {
    json shortcuts = json::object();
    for (const auto& [id, keys] : *prefs.shortcut_overrides) shortcuts[id] = keys;
    table["shortcuts"] = shortcuts;
  }
  if (prefs.hidden_sources) table["hidden_sources"] = *prefs.hidden_sources;
  if (prefs.source_order) table["source_order"] = *prefs.source_order;
  if (prefs.source_imported_at) {
    json imported = json::object();
    for (const auto& [id, at] : *prefs.source_imported_at) imported[id] = at;
    table["source_imported_at"] = imported;
  }
  if (prefs.sidebar_recent_count) table["sidebar_recent_count"] = *prefs.sidebar_recent_count;
  if (prefs.sidebar_source_counts) table["sidebar_source_counts"] = *prefs.sidebar_source_counts;
  if (prefs.sidebar_source_icons) table["sidebar_source_icons"] = *prefs.sidebar_source_icons;
  if (prefs.sidebar_pinned_style) table["sidebar_pinned_style"] = *prefs.sidebar_pinned_style;
  if (prefs.sidebar_recent_style) table["sidebar_recent_style"] = *prefs.sidebar_recent_style;
  if (prefs.sidebar_recent_when) table["sidebar_recent_when"] = *prefs.sidebar_recent_when;
  if (prefs.library_filter_tabs) table["library_filter_tabs"] = *prefs.library_filter_tabs;
  if (prefs.library_continue_row) table["library_continue_row"] = *prefs.library_continue_row;
  if (prefs.library_continue_count) table["library_continue_count"] = *prefs.library_continue_count;
  if (prefs.tile_status) table["tile_status"] = *prefs.tile_status;
  if (prefs.tile_source_mark) table["tile_source_mark"] = *prefs.tile_source_mark;
  if (prefs.tile_pin_badge) table["tile_pin_badge"] = *prefs.tile_pin_badge;
  if (prefs.source_page_tabs) table["source_page_tabs"] = *prefs.source_page_tabs;
  if (prefs.tile_size_synced) table["tile_size_synced"] = *prefs.tile_size_synced;
  if (prefs.source_tile_widths) table["source_tile_widths"] = *prefs.source_tile_widths;
  for (const std::string& key : prefs.clear) table[key] = nullptr;  // merge-patch: null deletes

  // Short, because SaveFrontendPrefsBlocking runs this on the UI thread
  // while a window is closing.
  const transport::Reply reply = transport::Patch("/v1/config", json{{"frontend", table}},
                                                  {.read_timeout = std::chrono::seconds(2)});
  return {reply.ok, reply.error};
}

ArtworkResult GetArtworkSync(const std::string& id, const std::string& slot) {
  ArtworkResult result;
  const transport::Blob blob =
      transport::GetBinary("/v1/games/" + id + "/artwork?type=" + slot);
  if (blob.status == 404) {
    result.missing = true;
    return result;
  }
  if (!blob.ok) {
    result.error = blob.error;
    return result;
  }
  result.ok = true;
  result.bytes = blob.bytes;
  result.content_type = blob.content_type;
  return result;
}

ArtCandidate ParseArtCandidate(const json& item) {
  ArtCandidate candidate;
  if (!item.is_object()) return candidate;
  candidate.id = item.value("id", std::int64_t{0});
  candidate.width = item.value("width", 0);
  candidate.height = item.value("height", 0);
  candidate.style = item.value("style", std::string());
  candidate.source = item.value("source", std::string("steamgriddb"));
  candidate.nsfw = item.value("nsfw", false);
  return candidate;
}

GameMetadataResult GetMetadataSync(const std::string& id) {
  GameMetadataResult result;
  const transport::Reply reply = transport::Get("/v1/games/" + id + "/metadata");
  if (reply.status == 404) {
    result.missing = true;
    return result;
  }
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }

  if (!reply.body.is_object()) {
    result.error = "mirad sent metadata that isn't a JSON object";
    return result;
  }
  result.ok = true;
  GameMetadata& out = result.metadata;
  out.source = reply.body.value("source", std::string());

  const auto strings = [](const json& array) {
    std::vector<std::string> values;
    if (!array.is_array()) return values;
    for (const json& item : array) {
      if (item.is_string()) values.push_back(item.get<std::string>());
    }
    return values;
  };

  // Every block is optional: which ones mirad cached depends on the source,
  // and on what that source had for this game.
  if (reply.body.contains("steam") && reply.body["steam"].is_object()) {
    const json& steam = reply.body["steam"];
    out.description = steam.value("short_description", std::string());
    out.release_date = steam.value("release_date", std::string());
    out.developers = strings(steam.value("developers", json::array()));
    out.genres = strings(steam.value("genres", json::array()));
    out.price = steam.value("price", std::string());
    out.metacritic_score = steam.value("metacritic_score", 0);
    out.website = steam.value("website", std::string());
    if (steam.contains("pc_requirements") && steam["pc_requirements"].is_object()) {
      const json& requirements = steam["pc_requirements"];
      out.requirements_min = requirements.value("minimum", std::string());
      out.requirements_rec = requirements.value("recommended", std::string());
    }
    if (steam.contains("dlc") && steam["dlc"].is_array()) {
      for (const json& item : steam["dlc"]) {
        if (item.is_number_integer()) out.dlc_ids.push_back(item.get<std::int64_t>());
      }
    }
    out.content_descriptors = strings(steam.value("content_descriptors", json::array()));
    out.achievements_total = steam.value("achievements_total", 0);
    out.screenshots = strings(steam.value("screenshots", json::array()));
    out.trailers = strings(steam.value("movies", json::array()));
  }
  // From Legendary's catalog cache: only a description and a developer.
  if (reply.body.contains("epic") && reply.body["epic"].is_object()) {
    const json& epic = reply.body["epic"];
    if (out.description.empty()) out.description = epic.value("description", std::string());
    if (const std::string developer = epic.value("developer", std::string()); !developer.empty() && out.developers.empty()) {
      out.developers.push_back(developer);
    }
  }
  if (reply.body.contains("steam_reviews") && reply.body["steam_reviews"].is_object()) {
    const json& reviews = reply.body["steam_reviews"];
    out.review_summary = reviews.value("score_description", std::string());
    out.review_total = reviews.value("total_reviews", 0);
  }
  if (reply.body.contains("protondb") && reply.body["protondb"].is_object()) {
    out.protondb_tier = reply.body["protondb"].value("tier", std::string());
  }
  // "artwork" is the cover slot under its pre-`hero` name; see docs/api.md.
  for (const char* key : {"artwork", "hero", "capsule", "header", "logo", "icon"}) {
    if (!reply.body.contains(key) || !reply.body[key].is_object()) continue;
    out.art_slots.push_back(std::string(key) == "artwork" ? "cover" : key);
  }

  const auto candidates = [&](const char* slot) {
    std::vector<ArtCandidate> list;
    if (!reply.body.contains("art_candidates") || !reply.body["art_candidates"].is_object() ||
        !reply.body["art_candidates"].contains(slot)) {
      return list;
    }
    for (const json& item : reply.body["art_candidates"][slot]) list.push_back(ParseArtCandidate(item));
    return list;
  };
  out.cover_candidates = candidates("cover");
  out.hero_candidates = candidates("hero");

  const auto active_id = [&](const char* key) -> std::optional<std::int64_t> {
    if (!reply.body.contains(key) || !reply.body[key].is_object() ||
        !reply.body[key].contains("candidate_id")) {
      return std::nullopt;
    }
    return reply.body[key].value("candidate_id", std::int64_t{0});
  };
  out.cover_active_candidate_id = active_id("artwork");
  out.hero_active_candidate_id = active_id("hero");
  return result;
}

MetadataRefreshResult RefreshMetadataSync(const std::string& id, bool announce) {
  const transport::Reply reply =
      transport::Post("/v1/games/" + id + "/metadata/refresh?announce=" + (announce ? "1" : "0"));
  return {reply.ok, reply.error};
}

void FillMetadataBatch(MetadataBatchResult& result, const json& body) {
  result.refreshed = body.value("refreshed", 0);
  result.failed = body.value("failed", 0);
}

ArtworkSelectResult SelectArtworkSync(const std::string& id, const std::string& slot,
                                      std::int64_t candidate_id) {
  const transport::Reply reply = transport::PostJson(
      "/v1/games/" + id + "/artwork?type=" + slot, json{{"candidate_id", candidate_id}});
  return {reply.ok, reply.error};
}

GameActionResult FetchArtCandidatesSync(const std::string& id, const std::string& slot, int page,
                                        const std::string& request) {
  const transport::Reply reply = transport::Post("/v1/games/" + id + "/artwork/candidates?type=" + slot +
                                                 "&page=" + std::to_string(page) + "&request=" + request);
  return {reply.ok, reply.error};
}

GameActionResult FetchArtThumbsSync(const std::string& id, const std::string& slot,
                                    const std::vector<std::int64_t>& candidate_ids) {
  const transport::Reply reply = transport::PostJson("/v1/games/" + id + "/artwork/thumbs?type=" + slot,
                                                     json{{"candidate_ids", candidate_ids}});
  return {reply.ok, reply.error};
}

ArtThumbsResult GetArtThumbsSync(const std::string& id, const std::string& slot,
                                 const std::vector<std::int64_t>& candidate_ids) {
  ArtThumbsResult result;
  for (const std::int64_t candidate_id : candidate_ids) {
    const transport::Blob blob = transport::GetBinary("/v1/games/" + id + "/artwork/thumb?type=" + slot +
                                                      "&candidate_id=" + std::to_string(candidate_id));
    if (blob.ok) result.images.emplace_back(candidate_id, blob.bytes);
  }
  return result;
}

RunnerCatalogResult GetRunnerCatalogSync(const std::string& kind, const std::string& source) {
  RunnerCatalogResult result;
  // Leaves the machine (GitHub releases), so the default timeout is nowhere
  // near enough.
  std::string path = "/v1/runners/catalog?kind=" + kind;
  if (!source.empty()) path += "&source=" + source;
  const transport::Reply reply = transport::Get(path, {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/catalog");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    RunnerRelease release;
    release.tag = entry.value("tag", std::string());
    release.name = entry.value("name", release.tag);
    release.label = entry.value("label", release.name);
    release.source = entry.value("source", std::string());
    release.installed = entry.value("installed", false);
    release.asset_name = entry.value("asset_name", std::string());
    release.size_bytes = entry.value("size_bytes", std::int64_t{0});
    release.published_at = entry.value("published_at", std::string());
    release.has_checksum = entry.value("has_checksum", false);
    result.releases.push_back(std::move(release));
  }
  return result;
}

RunnerDownloadResult DownloadRunnerSync(const std::string& kind, const std::string& tag,
                                       const std::string& source) {
  // Checks GitHub for the release before answering.
  const transport::Reply reply =
      transport::PostJson("/v1/runners/download", json{{"kind", kind}, {"tag", tag}, {"source", source}},
                          {.read_timeout = std::chrono::seconds(30)});
  return {reply.ok, reply.error};
}

RunnerSourcesResult ListRunnerSourcesSync(const std::string& kind) {
  RunnerSourcesResult result;
  const transport::Reply reply = transport::Get("/v1/runners/sources?kind=" + kind);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/sources");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    result.sources.push_back({entry.value("id", std::string()), entry.value("label", std::string())});
  }
  return result;
}

RunnerUpdatesResult GetRunnerUpdatesSync() {
  RunnerUpdatesResult result;
  const transport::Reply reply = transport::Get("/v1/runners/updates", {.read_timeout = std::chrono::seconds(60)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/updates");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    result.updates.push_back({entry.value("reference", std::string()), entry.value("source", std::string()),
                              entry.value("tag", std::string()), entry.value("name", std::string()),
                              entry.value("label", std::string())});
  }
  return result;
}

RunnerDownloadResult UpdateRunnerSync(const std::string& reference) {
  const transport::Reply reply = transport::PostJson("/v1/runners/update", json{{"reference", reference}},
                                                     {.read_timeout = std::chrono::seconds(60)});
  return {reply.ok, reply.error};
}

RunnerToolsResult ListRunnerToolsSync() {
  RunnerToolsResult result;
  const transport::Reply reply = transport::Get("/v1/runners/tools");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/tools");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    result.tools.push_back({entry.value("id", std::string()), entry.value("label", std::string()),
                            entry.value("doc", std::string()), entry.value("path", std::string()),
                            entry.value("installed", false)});
  }
  return result;
}

RunnerDownloadResult SetupRunnerToolSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/runners/tools/" + id + "/setup",
                                                 {.read_timeout = std::chrono::seconds(30)});
  return {reply.ok, reply.error};
}

void FillLutrisImport(LutrisImportResult& result, const json& body) {
  result.added = body.value("added", 0);
  result.updated = body.value("updated", 0);
  result.other_runner = body.value("other_runner", 0);
  result.incomplete = body.value("incomplete", 0);
}

// Steam scans, store and launcher imports all answer {added, updated}.
template <typename R>
void FillAddedUpdated(R& result, const json& body) {
  result.added = body.value("added", 0);
  result.updated = body.value("updated", 0);
}

RunInPrefixResult RunInPrefixSync(const std::string& id, const std::string& exe_path,
                                  const std::string& args) {
  const transport::Reply reply = transport::PostJson(
      "/v1/games/" + id + "/run", json{{"exe_path", exe_path}, {"args", args}},
      // Provisions a prefix on demand if there isn't one yet, which is
      // genuinely slow (it's initialising Wine/Proton, see
      // docs/architecture.md).
      {.read_timeout = std::chrono::seconds(120)});
  return {reply.ok, reply.error};
}

FinishInstallResult FinishInstallSync(const std::string& id, const std::string& install_path,
                                      const std::string& exe_path) {
  json body = json::object();
  if (!install_path.empty()) body["install_path"] = install_path;
  if (!exe_path.empty()) body["exe_path"] = exe_path;
  const transport::Reply reply = body.empty() ? transport::Post("/v1/games/" + id + "/finish-install")
                                              : transport::PostJson("/v1/games/" + id + "/finish-install", body);
  return {reply.ok, reply.error};
}

PatchGameConfigResult PatchGameConfigSync(const std::string& id,
                                          const std::vector<GameConfigEdit>& edits) {
  json body = json::object();
  for (const GameConfigEdit& edit : edits) {
    body[edit.key] =
        edit.clear ? json(nullptr) : mapping::TypedValueFromText(edit.type, edit.value);
  }
  const transport::Reply reply = transport::Patch("/v1/games/" + id + "/config", body);
  return {reply.ok, reply.error};
}

PatchGamesResult PatchGamesSync(const GamesPatch& patch) {
  PatchGamesResult result;
  json config = json::object();
  for (const GameConfigEdit& edit : patch.config) {
    config[edit.key] = edit.clear ? json(nullptr) : mapping::TypedValueFromText(edit.type, edit.value);
  }
  const json body = {{"ids", patch.ids},
                     {"add_tags", patch.add_tags},
                     {"remove_tags", patch.remove_tags},
                     {"config", config}};
  const transport::Reply reply = transport::Patch("/v1/games", body);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  if (reply.body.is_object() && reply.body.contains("games") && reply.body["games"].is_array()) {
    for (const json& entry : reply.body["games"]) result.games.push_back(mapping::ToGameSummary(entry));
  }
  return result;
}

GameLogResult GetGameLogSync(const std::string& id, int lines) {
  GameLogResult result;
  const transport::Reply reply =
      transport::Get("/v1/games/" + id + "/log?lines=" + std::to_string(lines));
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/games/" + id + "/log");
    return result;
  }

  result.ok = true;
  if (reply.body.contains("lines") && reply.body["lines"].is_array()) {
    for (const json& line : reply.body["lines"]) {
      if (line.is_string()) result.lines.push_back(line.get<std::string>());
    }
  }
  return result;
}

GameModeStatusResult GetGameModeStatusSync() {
  GameModeStatusResult result;
  const transport::Reply reply = transport::Get("/v1/gamemode/status");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.installed = reply.body.value("installed", false);
  result.daemon_running = reply.body.value("daemon_running", false);
  return result;
}

TricksResult RunWinetricksSync(const std::string& id, const std::string& verb) {
  const transport::Reply reply =
      transport::PostJson("/v1/games/" + id + "/tricks", json{{"verb", verb}});
  return {reply.ok, reply.error};
}

RunnerRemoveResult DeleteRunnerSync(const std::string& kind, const std::string& name) {
  const transport::Reply reply = transport::Delete("/v1/runners/" + kind + ":" + QueryEncode(name));
  return {reply.ok, reply.error};
}

GameDetailResult AddManualGameSync(const std::string& install_path, const std::string& exe_path,
                                   const std::string& name, const std::string& platform,
                                   bool is_installer) {
  json body{{"install_path", install_path}, {"exe_path", exe_path}, {"is_installer", is_installer}};
  if (!name.empty()) body["name"] = name;
  if (!platform.empty()) body["platform"] = platform;

  GameDetailResult result;
  const transport::Reply reply = transport::PostJson("/v1/games/manual", body);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("POST /v1/games/manual");
    return result;
  }

  result.ok = true;
  result.game = mapping::ToGameDetail(reply.body);
  return result;
}

DesktopEntryCandidatesResult GetDesktopEntryCandidatesSync() {
  DesktopEntryCandidatesResult result;
  const transport::Reply reply = transport::Get("/v1/desktop-entries/candidates");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/desktop-entries/candidates");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    DesktopEntryCandidate c;
    c.id = entry.value("id", std::string());
    c.name = entry.value("name", std::string());
    c.icon = entry.value("icon", std::string());
    result.candidates.push_back(std::move(c));
  }
  return result;
}

DesktopEntryImportResult ImportDesktopEntriesSync(const std::vector<std::string>& ids) {
  DesktopEntryImportResult result;
  const transport::Reply reply =
      transport::PostJson("/v1/desktop-entries/import", json{{"ids", ids}});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.added = reply.body.value("added", 0);
  result.updated = reply.body.value("updated", 0);
  return result;
}

DesktopEntrySyncResult SyncDesktopEntriesSync() {
  const transport::Reply reply = transport::Post("/v1/desktop-entries/sync");
  return {reply.ok, reply.error};
}

// Per-store endpoint names. Humble has no import or sign-out of its own.
struct StoreEndpoints {
  std::string status;
  std::string tool_key;  // the tool's object in the status reply
  std::string setup;
  std::string credential_field;
};

std::optional<StoreEndpoints> EndpointsFor(const std::string& source) {
  if (source == "epic") return StoreEndpoints{"/v1/epic/status", "legendary", "/v1/epic/legendary/install", "code"};
  if (source == "gog") return StoreEndpoints{"/v1/gog/status", "gogdl", "/v1/gog/setup", "code"};
  if (source == "itch") return StoreEndpoints{"/v1/itch/status", "butler", "/v1/itch/setup", "api_key"};
  if (source == "humble") {
    return StoreEndpoints{"/v1/humble/status", "humble_cli", "/v1/humble/setup", "session_key"};
  }
  if (source == "amazon") return StoreEndpoints{"/v1/amazon/status", "nile", "/v1/amazon/setup", "redirect"};
  return std::nullopt;
}

std::string UnknownStore(const std::string& source) { return "Unknown store \"" + source + "\"."; }

StoreStatusResult GetStoreStatusSync(const std::string& source) {
  StoreStatusResult result;
  const std::optional<StoreEndpoints> endpoints = EndpointsFor(source);
  if (!endpoints) {
    result.error = UnknownStore(source);
    return result;
  }
  // Humble's status asks humble-cli itself, over the network.
  const transport::Reply reply = transport::Get(endpoints->status, {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET " + endpoints->status);
    return result;
  }
  result.ok = true;
  const json tool = reply.body.value(endpoints->tool_key, json::object());
  if (tool.is_object()) {
    result.tool_installed = tool.value("installed", false);
    result.tool_version = tool.value("version", std::string());
  }
  result.authenticated = reply.body.value("authenticated", false);
  result.account = reply.body.value("account", std::string());
  result.login_url = reply.body.value("login_url", std::string());
  return result;
}

StoreActionResult SetupStoreToolSync(const std::string& source) {
  const std::optional<StoreEndpoints> endpoints = EndpointsFor(source);
  if (!endpoints) return {false, UnknownStore(source)};
  // Asks GitHub for the newest release before answering.
  const transport::Reply reply = transport::Post(endpoints->setup, {.read_timeout = std::chrono::seconds(30)});
  return {reply.ok, reply.error};
}

StoreActionResult SignInStoreSync(const std::string& source, const std::string& credential) {
  const std::optional<StoreEndpoints> endpoints = EndpointsFor(source);
  if (!endpoints) return {false, UnknownStore(source)};
  const transport::Reply reply = transport::PostJson("/v1/" + source + "/auth",
                                                     {{endpoints->credential_field, credential}},
                                                     {.read_timeout = std::chrono::seconds(60)});
  return {reply.ok, reply.error};
}

StoreActionResult SignOutStoreSync(const std::string& source) {
  const transport::Reply reply = transport::Post("/v1/" + source + "/logout");
  return {reply.ok, reply.error};
}

StoreLibraryResult GetStoreLibrarySync(const std::string& source) {
  StoreLibraryResult result;
  const transport::Reply reply =
      transport::Get("/v1/library?source=" + source, {.read_timeout = std::chrono::seconds(60)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/library");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    if (!entry.is_object()) continue;
    result.titles.push_back({.ref = entry.value("ref", std::string()),
                             .title = entry.value("title", std::string()),
                             .installed = entry.value("installed", false),
                             .owned = entry.value("owned", true)});
  }
  return result;
}

StoreActionResult InstallStoreTitleSync(const std::string& source, const std::string& ref, bool update) {
  const transport::Reply reply = transport::PostJson(update ? "/v1/library/update" : "/v1/library/install",
                                                     {{"source", source}, {"ref", ref}});
  return {reply.ok, reply.error};
}

HumbleLibraryResult GetHumbleLibrarySync() {
  HumbleLibraryResult result;
  const transport::Reply reply = transport::Get("/v1/humble/library", {.read_timeout = std::chrono::seconds(60)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/humble/library");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    if (!entry.is_object()) continue;
    result.bundles.push_back({.key = entry.value("key", std::string()),
                              .name = entry.value("name", std::string()),
                              .claimed = entry.value("claimed", false)});
  }
  return result;
}

StoreActionResult DownloadHumbleBundleSync(const std::string& bundle_key) {
  const transport::Reply reply = transport::PostJson("/v1/humble/download", {{"bundle_key", bundle_key}});
  return {reply.ok, reply.error};
}

GriddbMatchesResult GetGriddbMatchesSync(const std::string& id, const std::string& query) {
  GriddbMatchesResult result;
  std::string url = "/v1/games/" + id + "/metadata/matches";
  if (!query.empty()) url += "?q=" + QueryEncode(query);
  const transport::Reply reply = transport::Get(url, {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.query = reply.body.value("query", std::string());
  result.chosen = reply.body.value("chosen", std::int64_t{0});
  for (const json& entry : reply.body.value("matches", json::array())) {
    GriddbMatch match;
    match.id = entry.value("id", std::int64_t{0});
    match.name = entry.value("name", std::string());
    if (const std::int64_t released = entry.value("release_date", std::int64_t{0}); released > 0) {
      const auto day = std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds{std::chrono::seconds{released}});
      match.year = static_cast<int>(std::chrono::year_month_day{day}.year());
    }
    result.matches.push_back(std::move(match));
  }
  return result;
}

GameActionResult SetGriddbMatchSync(const std::string& id, std::int64_t griddb_id) {
  const transport::Reply reply =
      transport::PostJson("/v1/games/" + id + "/metadata/match", {{"steamgriddb_id", griddb_id}});
  return {reply.ok, reply.error};
}

ArtworkResult GetTitleArtworkSync(const std::string& source, const std::string& ref) {
  ArtworkResult result;
  const transport::Blob blob =
      transport::GetBinary("/v1/library/artwork?source=" + QueryEncode(source) + "&ref=" + QueryEncode(ref));
  if (blob.status == 404) {
    result.missing = true;
    return result;
  }
  if (!blob.ok) {
    result.error = blob.error;
    return result;
  }
  result.ok = true;
  result.bytes = blob.bytes;
  result.content_type = blob.content_type;
  return result;
}

StoreActionResult QueueTitleArtworkSync(const std::string& source, const std::vector<StoreTitle>& titles) {
  json list = json::array();
  for (const StoreTitle& title : titles) list.push_back({{"ref", title.ref}, {"title", title.title}});
  const transport::Reply reply = transport::PostJson("/v1/library/artwork", {{"source", source}, {"titles", list}});
  return {reply.ok, reply.error};
}

RemovalPlanResult GetRemovalPlanSync(const std::string& source) {
  RemovalPlanResult result;
  const transport::Reply reply = transport::Get("/v1/sources/" + source + "/removal");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  for (const json& game : reply.body.value("games", json::array())) {
    result.games.push_back({.id = game.value("id", std::string()),
                            .name = game.value("name", std::string()),
                            .deletes = game.value("deletes", std::string())});
  }
  result.launcher_dir = reply.body.value("launcher_dir", std::string());
  for (const json& path : reply.body.value("kept", json::array())) {
    if (path.is_string()) result.kept.push_back(path.get<std::string>());
  }
  result.signs_out = reply.body.value("signs_out", false);
  return result;
}

void FillRemoveSource(RemoveSourceResult& result, const json& body) {
  result.removed = body.value("removed", 0);
  for (const json& problem : body.value("problems", json::array())) {
    if (problem.is_string()) result.problems.push_back(problem.get<std::string>());
  }
}

SourceRunnerResult SourceRunnerFrom(const transport::Reply& reply) {
  SourceRunnerResult result;
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.runner_ref = reply.body.value("runner_ref", std::string());
  result.games = reply.body.value("games", 0);
  result.differing = reply.body.value("differing", 0);
  return result;
}

SourceRunnerResult GetSourceRunnerSync(const std::string& source) {
  return SourceRunnerFrom(transport::Get("/v1/sources/" + source + "/runner"));
}

SourceRunnerResult SetSourceRunnerSync(const std::string& source, const std::string& runner_ref,
                                       bool apply_to_games) {
  return SourceRunnerFrom(transport::PostJson(
      "/v1/sources/" + source + "/runner", {{"runner_ref", runner_ref}, {"apply_to_games", apply_to_games}}));
}

ItchCollectionsResult GetItchCollectionsSync() {
  ItchCollectionsResult result;
  const transport::Reply reply = transport::Get("/v1/itch/collections");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/itch/collections");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    if (!entry.is_object()) continue;
    result.collections.push_back({.id = entry.value("id", std::int64_t{0}),
                                  .title = entry.value("title", std::string()),
                                  .games_count = entry.value("games_count", std::int64_t{0}),
                                  .own = entry.value("own", false)});
  }
  return result;
}

StoreActionResult AddItchCollectionSync(const std::string& link) {
  const transport::Reply reply = transport::PostJson("/v1/itch/collections", {{"link", link}});
  return {reply.ok, reply.error};
}

StoreActionResult RemoveItchCollectionSync(std::int64_t id) {
  const transport::Reply reply = transport::Delete("/v1/itch/collections/" + std::to_string(id));
  return {reply.ok, reply.error};
}

InstallerInfoResult GetInstallerInfoSync(const std::string& id, const std::string& path) {
  InstallerInfoResult result;
  std::string url = "/v1/games/" + id + "/installer";
  if (!path.empty()) url += "?path=" + QueryEncode(path);
  const transport::Reply reply = transport::Get(url);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.path = reply.body.value("path", std::string());
  result.size_bytes = reply.body.value("size_bytes", std::int64_t{0});
  result.format = reply.body.value("format", std::string("unknown"));
  result.silent = reply.body.value("silent", false);
  return result;
}

GameActionResult InstallGameSync(const std::string& id, bool interactive, const std::string& installer) {
  json body = {{"interactive", interactive}};
  if (!installer.empty()) body["installer"] = installer;
  const transport::Reply reply = transport::PostJson("/v1/games/" + id + "/install", body);
  return {reply.ok, reply.error};
}

InstallProgressResult GetInstallProgressSync(const std::string& id) {
  InstallProgressResult result;
  const transport::Reply reply = transport::Get("/v1/games/" + id + "/install/progress");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.state = reply.body.value("state", std::string("idle"));
  result.bytes_written = reply.body.value("bytes_written", std::int64_t{0});
  return result;
}

std::vector<GameFailure> ToGameFailures(const json& reply, const char* key) {
  std::vector<GameFailure> out;
  if (!reply.is_object() || !reply.contains(key) || !reply[key].is_array()) return out;
  for (const json& entry : reply[key]) {
    if (!entry.is_object()) continue;
    out.push_back({entry.value("id", std::string()), mapping::ToApiError(entry.value("error", json::object()))});
  }
  return out;
}

void FillRelocate(RelocateLibraryResult& result, const json& body) {
  result.moved = body.value("moved", 0);
  result.failed = body.value("failed", 0);
  result.errors = ToGameFailures(body, "errors");
}

// No `ids` relocates every game.
void RelocateLibraryJob(QObject* context, std::optional<std::vector<std::string>> ids,
                        std::function<void(RelocateLibraryResult)> callback) {
  RunJob<RelocateLibraryResult>(
      context, "relocate",
      [ids](const std::string& query) {
        return ids ? transport::PostJson("/v1/library/relocate" + query, {{"ids", *ids}})
                   : transport::Post("/v1/library/relocate" + query);
      },
      FillRelocate, std::move(callback));
}

void FillDeleteGames(DeleteGamesResult& result, const json& body) {
  if (body.is_object() && body.contains("removed") && body["removed"].is_array()) {
    for (const json& id : body["removed"]) {
      if (id.is_string()) result.removed.push_back(id.get<std::string>());
    }
  }
  result.failed = ToGameFailures(body, "failed");
}

LoginUrlResult BeginAmazonLoginSync() {
  LoginUrlResult result;
  const transport::Reply reply = transport::Post("/v1/amazon/login", {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.url = reply.body.value("url", std::string());
  result.ok = !result.url.empty();
  if (!result.ok) result.error = transport::UnexpectedResponse("POST /v1/amazon/login");
  return result;
}

LaunchersResult GetLaunchersSync() {
  LaunchersResult result;
  const transport::Reply reply = transport::Get("/v1/launchers");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/launchers");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    if (!entry.is_object()) continue;
    result.launchers.push_back({.id = entry.value("id", std::string()),
                                .name = entry.value("name", std::string()),
                                .game_id = entry.value("game_id", std::string()),
                                .installed = entry.value("installed", false),
                                .install_state = entry.value("install_state", std::string()),
                                .interactive_install = entry.value("interactive_install", false),
                                .prefix = entry.value("prefix", std::string()),
                                .runner_ref = entry.value("runner_ref", std::string()),
                                .error = entry.value("error", std::string())});
  }
  return result;
}

StoreActionResult InstallLauncherSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/launchers/" + id + "/install");
  return {reply.ok, reply.error};
}

StoreActionResult OpenLauncherSync(const std::string& id) {
  const transport::Reply reply = transport::PostJson("/v1/launchers/" + id + "/open", json::object());
  return {reply.ok, reply.error};
}

}  // namespace

std::string MiradClient::ResolveSocketPath() { return transport::SocketPath(); }

void MiradClient::CheckHealthAsync(QObject* context, std::function<void(HealthStatus)> callback) {
  async::Run(context, [] { return GetHealthSync(); }, std::move(callback));
}

void MiradClient::ListAllGamesAsync(QObject* context, std::function<void(GamesResult)> callback) {
  async::Run(context, [] { return GetGamesSync(std::string(), std::string(), true); }, std::move(callback));
}

void MiradClient::DeleteGameAsync(QObject* context, const std::string& id, bool delete_files,
                                  bool delete_prefix, bool delete_metadata,
                                  std::function<void(DeleteResult)> callback) {
  async::Run(
      context, [id, delete_files, delete_prefix, delete_metadata] {
        return DeleteGameSync(id, delete_files, delete_prefix, delete_metadata);
      },
      std::move(callback), async::Lane::Slow);
}

void MiradClient::LaunchGameAsync(QObject* context, const std::string& id,
                                  std::function<void(LaunchResult)> callback) {
  async::Run(context, [id] { return LaunchGameSync(id); }, std::move(callback));
}

void MiradClient::StopGameAsync(QObject* context, const std::string& id,
                                std::function<void(StopResult)> callback) {
  async::Run(context, [id] { return StopGameSync(id); }, std::move(callback));
}

void MiradClient::ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback) {
  RunJob<ScanResult>(
      context, "scan", [](const std::string& query) { return transport::Post("/v1/library/scan" + query); },
      FillScan, std::move(callback));
}

void MiradClient::GetGameAsync(QObject* context, const std::string& id,
                               std::function<void(GameDetailResult)> callback) {
  async::Run(context, [id] { return GetGameSync(id); }, std::move(callback));
}

void MiradClient::PatchGameAsync(QObject* context, const std::string& id, const GamePatch& patch,
                                 std::function<void(PatchGameResult)> callback) {
  async::Run(context, [id, patch] { return PatchGameSync(id, patch); }, std::move(callback));
}

void MiradClient::GetConfigSchemaAsync(QObject* context,
                                       std::function<void(ConfigSchemaResult)> callback) {
  async::Run(context, [] { return GetConfigSchemaSync(); }, std::move(callback));
}

void MiradClient::GetConfigAsync(QObject* context, std::function<void(ConfigResult)> callback) {
  async::Run(context, [] { return GetConfigSync(); }, std::move(callback));
}

void MiradClient::PatchConfigAsync(QObject* context, const std::vector<ConfigEdit>& edits,
                                   std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [edits] { return PatchConfigSync(edits); }, std::move(callback));
}

void MiradClient::ResetConfigKeyAsync(QObject* context, const std::string& key,
                                      std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [key] { return ResetConfigKeySync(key); }, std::move(callback));
}

void MiradClient::ListRunnersAsync(QObject* context, std::function<void(RunnersResult)> callback) {
  async::Run(context, [] { return GetRunnersSync(); }, std::move(callback));
}

void MiradClient::GetFrontendPrefsAsync(QObject* context,
                                        std::function<void(FrontendPrefsResult)> callback) {
  async::Run(context, [] { return GetFrontendPrefsSync(); }, std::move(callback));
}

void MiradClient::SaveFrontendPrefsAsync(QObject* context, const FrontendPrefs& prefs,
                                         std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [prefs] { return SaveFrontendPrefsSync(prefs); }, std::move(callback));
}

PatchConfigResult MiradClient::SaveFrontendPrefsBlocking(const FrontendPrefs& prefs) {
  return SaveFrontendPrefsSync(prefs);
}

void MiradClient::ClearArtThumbsBlocking() {
  transport::Delete("/v1/artwork/thumbs", {.read_timeout = std::chrono::seconds(2)});
}

FrontendPrefsResult MiradClient::GetFrontendPrefsBlocking() { return GetFrontendPrefsSync(); }

void MiradClient::GetArtworkImageAsync(QObject* context, const std::string& id, const std::string& slot,
                                       std::function<void(QImage)> callback) {
  async::Run<QImage>(
      context, [id, slot] {
        const ArtworkResult result = GetArtworkSync(id, slot);
        return result.ok ? DecodeImage(result.bytes) : QImage();
      },
      std::move(callback));
}

void MiradClient::GetMetadataAsync(QObject* context, const std::string& id,
                                   std::function<void(GameMetadataResult)> callback) {
  async::Run(context, [id] { return GetMetadataSync(id); }, std::move(callback));
}

void MiradClient::RefreshMetadataAsync(QObject* context, const std::string& id, bool announce,
                                       std::function<void(MetadataRefreshResult)> callback) {
  async::Run(context, [id, announce] { return RefreshMetadataSync(id, announce); }, std::move(callback));
}

void MiradClient::RefreshMetadataManyAsync(QObject* context, const std::vector<std::string>& ids,
                                           std::function<void(MetadataBatchResult)> callback) {
  const json body = {{"ids", ids}};
  RunJob<MetadataBatchResult>(
      context, "metadata",
      [body](const std::string& query) { return transport::PostJson("/v1/games/metadata/refresh" + query, body); },
      FillMetadataBatch, std::move(callback));
}

void MiradClient::SelectArtworkAsync(QObject* context, const std::string& id, const std::string& slot,
                                     std::int64_t candidate_id,
                                     std::function<void(ArtworkSelectResult)> callback) {
  async::Run(context, [id, slot, candidate_id] { return SelectArtworkSync(id, slot, candidate_id); },
             std::move(callback));
}

void MiradClient::FetchArtCandidatesAsync(QObject* context, const std::string& id, const std::string& slot, int page,
                                          const std::string& request,
                                          std::function<void(GameActionResult)> callback) {
  async::Run(context, [id, slot, page, request] { return FetchArtCandidatesSync(id, slot, page, request); },
             std::move(callback));
}

void MiradClient::FetchArtThumbsAsync(QObject* context, const std::string& id, const std::string& slot,
                                      const std::vector<std::int64_t>& candidate_ids,
                                      std::function<void(GameActionResult)> callback) {
  async::Run(context, [id, slot, candidate_ids] { return FetchArtThumbsSync(id, slot, candidate_ids); },
             std::move(callback));
}

void MiradClient::GetArtThumbsAsync(QObject* context, const std::string& id, const std::string& slot,
                                    const std::vector<std::int64_t>& candidate_ids,
                                    std::function<void(std::vector<std::pair<std::int64_t, QImage>>)> callback) {
  async::Run<std::vector<std::pair<std::int64_t, QImage>>>(
      context, [id, slot, candidate_ids] {
        std::vector<std::pair<std::int64_t, QImage>> images;
        for (const auto& [candidate_id, bytes] : GetArtThumbsSync(id, slot, candidate_ids).images) {
          if (QImage image = DecodeImage(bytes); !image.isNull()) images.emplace_back(candidate_id, std::move(image));
        }
        return images;
      },
      std::move(callback));
}

void MiradClient::RefreshMissingArtworkAsync(QObject* context,
                                             std::function<void(MetadataBatchResult)> callback) {
  RunJob<MetadataBatchResult>(
      context, "metadata",
      [](const std::string& query) { return transport::Post("/v1/games/metadata/refresh-missing" + query); },
      FillMetadataBatch, std::move(callback));
}

void MiradClient::GetRunnerCatalogAsync(QObject* context, const std::string& kind, const std::string& source,
                                        std::function<void(RunnerCatalogResult)> callback) {
  async::Run(context, [kind, source] { return GetRunnerCatalogSync(kind, source); }, std::move(callback),
             async::Lane::Slow);
}

void MiradClient::DownloadRunnerAsync(QObject* context, const std::string& kind,
                                      const std::string& tag, const std::string& source,
                                      std::function<void(RunnerDownloadResult)> callback) {
  async::Run(context, [kind, tag, source] { return DownloadRunnerSync(kind, tag, source); }, std::move(callback),
             async::Lane::Slow);
}

void MiradClient::ListRunnerSourcesAsync(QObject* context, const std::string& kind,
                                         std::function<void(RunnerSourcesResult)> callback) {
  async::Run(context, [kind] { return ListRunnerSourcesSync(kind); }, std::move(callback));
}

void MiradClient::GetRunnerUpdatesAsync(QObject* context, std::function<void(RunnerUpdatesResult)> callback) {
  async::Run(context, [] { return GetRunnerUpdatesSync(); }, std::move(callback), async::Lane::Slow);
}

void MiradClient::UpdateRunnerAsync(QObject* context, const std::string& reference,
                                    std::function<void(RunnerDownloadResult)> callback) {
  async::Run(context, [reference] { return UpdateRunnerSync(reference); }, std::move(callback), async::Lane::Slow);
}

void MiradClient::ListRunnerToolsAsync(QObject* context, std::function<void(RunnerToolsResult)> callback) {
  async::Run(context, [] { return ListRunnerToolsSync(); }, std::move(callback));
}

void MiradClient::SetupRunnerToolAsync(QObject* context, const std::string& id,
                                       std::function<void(RunnerDownloadResult)> callback) {
  async::Run(context, [id] { return SetupRunnerToolSync(id); }, std::move(callback), async::Lane::Slow);
}

void MiradClient::ScanSteamAsync(QObject* context, std::function<void(SteamScanResult)> callback) {
  RunJob<SteamScanResult>(
      context, "scan", [](const std::string& query) { return transport::Post("/v1/steam/scan" + query); },
      FillAddedUpdated<SteamScanResult>, std::move(callback));
}

void MiradClient::ImportLutrisAsync(QObject* context,
                                    std::function<void(LutrisImportResult)> callback) {
  RunJob<LutrisImportResult>(
      context, "import", [](const std::string& query) { return transport::Post("/v1/lutris/import" + query); },
      FillLutrisImport, std::move(callback));
}

void MiradClient::RunInPrefixAsync(QObject* context, const std::string& id,
                                   const std::string& exe_path, const std::string& args,
                                   std::function<void(RunInPrefixResult)> callback) {
  async::Run(context, [id, exe_path, args] { return RunInPrefixSync(id, exe_path, args); },
             std::move(callback), async::Lane::Slow);
}

void MiradClient::FinishInstallAsync(QObject* context, const std::string& id,
                                     std::function<void(FinishInstallResult)> callback,
                                     const std::string& install_path, const std::string& exe_path) {
  async::Run(context, [id, install_path, exe_path] { return FinishInstallSync(id, install_path, exe_path); }, std::move(callback));
}

void MiradClient::GetGameConfigAsync(QObject* context, const std::string& id,
                                     std::function<void(GameConfigResult)> callback) {
  async::Run(context, [id] { return GetGameConfigSync(id); }, std::move(callback));
}

void MiradClient::PatchGameConfigAsync(QObject* context, const std::string& id,
                                       const std::vector<GameConfigEdit>& edits,
                                       std::function<void(PatchGameConfigResult)> callback) {
  async::Run(context, [id, edits] { return PatchGameConfigSync(id, edits); }, std::move(callback));
}

void MiradClient::PatchGamesAsync(QObject* context, const GamesPatch& patch,
                                  std::function<void(PatchGamesResult)> callback) {
  async::Run(context, [patch] { return PatchGamesSync(patch); }, std::move(callback));
}

void MiradClient::GetGameLogAsync(QObject* context, const std::string& id, int lines,
                                  std::function<void(GameLogResult)> callback) {
  async::Run(context, [id, lines] { return GetGameLogSync(id, lines); }, std::move(callback));
}

void MiradClient::GetGameModeStatusAsync(QObject* context,
                                         std::function<void(GameModeStatusResult)> callback) {
  async::Run(context, [] { return GetGameModeStatusSync(); }, std::move(callback));
}

void MiradClient::RunWinetricksAsync(QObject* context, const std::string& id,
                                     const std::string& verb,
                                     std::function<void(TricksResult)> callback) {
  async::Run(context, [id, verb] { return RunWinetricksSync(id, verb); }, std::move(callback));
}

void MiradClient::DeleteRunnerAsync(QObject* context, const std::string& kind,
                                    const std::string& name,
                                    std::function<void(RunnerRemoveResult)> callback) {
  async::Run(context, [kind, name] { return DeleteRunnerSync(kind, name); }, std::move(callback));
}

void MiradClient::AddManualGameAsync(QObject* context, const std::string& install_path,
                                     const std::string& exe_path, const std::string& name,
                                     const std::string& platform, bool is_installer,
                                     std::function<void(GameDetailResult)> callback) {
  async::Run(
      context,
      [install_path, exe_path, name, platform, is_installer] {
        return AddManualGameSync(install_path, exe_path, name, platform, is_installer);
      },
      std::move(callback));
}

void MiradClient::GetDesktopEntryCandidatesAsync(
    QObject* context, std::function<void(DesktopEntryCandidatesResult)> callback) {
  async::Run(context, [] { return GetDesktopEntryCandidatesSync(); }, std::move(callback));
}

void MiradClient::ImportDesktopEntriesAsync(QObject* context, const std::vector<std::string>& ids,
                                            std::function<void(DesktopEntryImportResult)> callback) {
  async::Run(context, [ids] { return ImportDesktopEntriesSync(ids); }, std::move(callback));
}

void MiradClient::SyncDesktopEntriesAsync(QObject* context,
                                          std::function<void(DesktopEntrySyncResult)> callback) {
  async::Run(context, [] { return SyncDesktopEntriesSync(); }, std::move(callback));
}

bool MiradClient::ParseGameSummary(const std::string& data, GameSummary* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  // An id is what makes this a game record. Without this check any JSON
  // object at all parsed as a game with every field empty.
  if (!entry.contains("id") || !entry["id"].is_string() || entry["id"].get<std::string>().empty()) {
    return false;
  }
  *out = mapping::ToGameSummary(entry);
  return true;
}

bool MiradClient::ParseGameSummaries(const std::string& data, std::vector<GameSummary>* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object() || !entry.contains("games") || !entry["games"].is_array()) {
    return false;
  }
  out->clear();
  for (const json& game : entry["games"]) {
    if (mapping::Str(game, "id") != "") out->push_back(mapping::ToGameSummary(game));
  }
  return true;
}

bool MiradClient::ParseGameState(const std::string& data, GameStateEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->id = entry.value("id", std::string());
  out->state = entry.value("state", std::string());
  out->played_seconds = entry.value("played_seconds", std::int64_t{0});
  out->error = mapping::ToApiError(entry);
  return !out->id.empty();
}

bool MiradClient::ParseGameLaunched(const std::string& data, GameLaunchedEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->id = entry.value("id", std::string());
  if (out->id.empty()) return false;
  out->tracked = entry.value("tracked", false);
  return true;
}

bool MiradClient::ParseInstallDetected(const std::string& data, InstallDetectedEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->id = mapping::Str(entry, "id");
  out->install_path = mapping::Str(entry, "install_path");
  out->exe_path = mapping::Str(entry, "exe_path");
  return !out->id.empty() && !out->install_path.empty();
}

std::string MiradClient::ParseRemovedId(const std::string& data) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return {};
  return entry.value("id", std::string());
}

std::vector<std::string> MiradClient::ParseRemovedIds(const std::string& data) {
  const json entry = json::parse(data, nullptr, false);
  std::vector<std::string> ids;
  if (entry.is_discarded() || !entry.is_object() || !entry.contains("ids") || !entry["ids"].is_array()) return ids;
  for (const json& id : entry["ids"]) {
    if (id.is_string() && !id.get<std::string>().empty()) ids.push_back(id.get<std::string>());
  }
  return ids;
}

bool MiradClient::ParseMetadataEvent(const std::string& data, MetadataEvent* out) {
  const json payload = json::parse(data, nullptr, false);
  if (!payload.is_object()) return false;
  const std::string id = payload.value("id", std::string());
  if (id.empty()) return false;
  out->id = id;
  out->code = payload.value("code", std::string());
  out->error = mapping::ToApiError(payload);
  out->art = mapping::ToArtVersions(payload);
  return true;
}

bool MiradClient::ParseArtworkSelectEvent(const std::string& data, ArtworkSelectEvent* out) {
  const json payload = json::parse(data, nullptr, false);
  if (!payload.is_object()) return false;
  const std::string id = payload.value("id", std::string());
  if (id.empty()) return false;
  out->id = id;
  out->slot = payload.value("type", std::string());
  out->error = mapping::ToApiError(payload);
  out->art = mapping::ToArtVersions(payload);
  return true;
}

bool MiradClient::ParseArtCandidatesEvent(const std::string& data, ArtCandidatesEvent* out) {
  const json payload = json::parse(data, nullptr, false);
  if (!payload.is_object()) return false;
  out->id = payload.value("id", std::string());
  if (out->id.empty()) return false;
  out->slot = payload.value("type", std::string());
  out->page = payload.value("page", 0);
  out->request = payload.value("request", std::string());
  out->total = payload.value("total", 0);
  out->code = payload.value("code", std::string());
  out->error = mapping::ToApiError(payload);
  out->candidates.clear();
  if (payload.contains("candidates") && payload["candidates"].is_array()) {
    for (const json& item : payload["candidates"]) out->candidates.push_back(ParseArtCandidate(item));
  }
  return true;
}

bool MiradClient::ParseArtThumbsEvent(const std::string& data, ArtThumbsEvent* out) {
  const json payload = json::parse(data, nullptr, false);
  if (!payload.is_object()) return false;
  out->id = payload.value("id", std::string());
  if (out->id.empty()) return false;
  out->slot = payload.value("type", std::string());
  out->error = mapping::ToApiError(payload);
  const auto ids = [&](const char* key) {
    std::vector<std::int64_t> list;
    if (!payload.contains(key) || !payload[key].is_array()) return list;
    for (const json& item : payload[key]) {
      if (item.is_number_integer()) list.push_back(item.get<std::int64_t>());
    }
    return list;
  };
  out->ready = ids("ready");
  out->failed = ids("failed");
  return true;
}

bool MiradClient::ParseNotification(const std::string& data, NotificationEvent* out) {
  const json payload = json::parse(data, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) return false;
  out->message = payload.value("message", std::string());
  if (out->message.empty()) return false;
  out->level = payload.value("level", std::string("info"));
  return true;
}

void MiradClient::GetStoreStatusAsync(QObject* context, const std::string& source,
                                      std::function<void(StoreStatusResult)> callback) {
  async::Run(context, [source] { return GetStoreStatusSync(source); }, std::move(callback), async::Lane::Slow);
}

void MiradClient::SetupStoreToolAsync(QObject* context, const std::string& source,
                                      std::function<void(StoreActionResult)> callback) {
  async::Run(context, [source] { return SetupStoreToolSync(source); }, std::move(callback), async::Lane::Slow);
}

void MiradClient::SignInStoreAsync(QObject* context, const std::string& source,
                                   const std::string& credential,
                                   std::function<void(StoreActionResult)> callback) {
  async::Run(context, [source, credential] { return SignInStoreSync(source, credential); },
             std::move(callback), async::Lane::Slow);
}

void MiradClient::SignOutStoreAsync(QObject* context, const std::string& source,
                                    std::function<void(StoreActionResult)> callback) {
  async::Run(context, [source] { return SignOutStoreSync(source); }, std::move(callback));
}

void MiradClient::ImportStoreAsync(QObject* context, const std::string& source,
                                   std::function<void(StoreImportResult)> callback) {
  RunJob<StoreImportResult>(
      context, "import",
      [source](const std::string& query) { return transport::Post("/v1/" + source + "/import" + query); },
      FillAddedUpdated<StoreImportResult>, std::move(callback));
}

void MiradClient::GetStoreLibraryAsync(QObject* context, const std::string& source,
                                       std::function<void(StoreLibraryResult)> callback) {
  async::Run(context, [source] { return GetStoreLibrarySync(source); }, std::move(callback), async::Lane::Slow);
}

void MiradClient::InstallStoreTitleAsync(QObject* context, const std::string& source,
                                         const std::string& ref, bool update,
                                         std::function<void(StoreActionResult)> callback) {
  async::Run(context, [source, ref, update] { return InstallStoreTitleSync(source, ref, update); },
             std::move(callback));
}

void MiradClient::GetGriddbMatchesAsync(QObject* context, const std::string& id, const std::string& query,
                                        std::function<void(GriddbMatchesResult)> callback) {
  async::Run(context, [id, query] { return GetGriddbMatchesSync(id, query); }, std::move(callback), async::Lane::Slow);
}

void MiradClient::SetGriddbMatchAsync(QObject* context, const std::string& id, std::int64_t griddb_id,
                                      std::function<void(GameActionResult)> callback) {
  async::Run(context, [id, griddb_id] { return SetGriddbMatchSync(id, griddb_id); }, std::move(callback));
}

void MiradClient::QueueTitleArtworkAsync(QObject* context, const std::string& source,
                                         std::vector<StoreTitle> titles,
                                         std::function<void(StoreActionResult)> callback) {
  async::Run(context, [source, titles = std::move(titles)] { return QueueTitleArtworkSync(source, titles); },
             std::move(callback));
}

void MiradClient::GetRemovalPlanAsync(QObject* context, const std::string& source,
                                      std::function<void(RemovalPlanResult)> callback) {
  async::Run(context, [source] { return GetRemovalPlanSync(source); }, std::move(callback));
}

void MiradClient::RemoveSourceAsync(QObject* context, const std::string& source,
                                    std::function<void(RemoveSourceResult)> callback) {
  RunJob<RemoveSourceResult>(
      context, "remove_source",
      [source](const std::string& query) { return transport::Post("/v1/sources/" + source + "/remove" + query); },
      FillRemoveSource, std::move(callback));
}

void MiradClient::GetSourceRunnerAsync(QObject* context, const std::string& source,
                                       std::function<void(SourceRunnerResult)> callback) {
  async::Run(context, [source] { return GetSourceRunnerSync(source); }, std::move(callback));
}

void MiradClient::SetSourceRunnerAsync(QObject* context, const std::string& source,
                                       const std::string& runner_ref, bool apply_to_games,
                                       std::function<void(SourceRunnerResult)> callback) {
  async::Run(
      context, [source, runner_ref, apply_to_games] { return SetSourceRunnerSync(source, runner_ref, apply_to_games); },
      std::move(callback));
}

void MiradClient::GetItchCollectionsAsync(QObject* context,
                                          std::function<void(ItchCollectionsResult)> callback) {
  async::Run(context, [] { return GetItchCollectionsSync(); }, std::move(callback));
}

void MiradClient::AddItchCollectionAsync(QObject* context, const std::string& link,
                                         std::function<void(StoreActionResult)> callback) {
  async::Run(context, [link] { return AddItchCollectionSync(link); }, std::move(callback));
}

void MiradClient::RemoveItchCollectionAsync(QObject* context, std::int64_t id,
                                            std::function<void(StoreActionResult)> callback) {
  async::Run(context, [id] { return RemoveItchCollectionSync(id); }, std::move(callback));
}

bool MiradClient::ParseTitleArtworkEvent(const std::string& event_type, const std::string& data,
                                         StoreEvent* out) {
  constexpr std::string_view kPrefix = "library.artwork_";
  if (!event_type.starts_with(kPrefix)) return false;
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->kind = "artwork";
  out->state = event_type.substr(kPrefix.size());  // "ready" | "failed"
  out->source = entry.value("source", std::string());
  out->ref = entry.value("ref", std::string());
  out->error = mapping::ToApiError(entry);
  return !out->ref.empty();
}

void MiradClient::GetHumbleLibraryAsync(QObject* context,
                                        std::function<void(HumbleLibraryResult)> callback) {
  async::Run(context, [] { return GetHumbleLibrarySync(); }, std::move(callback), async::Lane::Slow);
}

void MiradClient::DownloadHumbleBundleAsync(QObject* context, const std::string& bundle_key,
                                            std::function<void(StoreActionResult)> callback) {
  async::Run(context, [bundle_key] { return DownloadHumbleBundleSync(bundle_key); },
             std::move(callback));
}

void MiradClient::GetInstallerInfoAsync(QObject* context, const std::string& id, const std::string& path,
                                        std::function<void(InstallerInfoResult)> callback) {
  async::Run(context, [id, path] { return GetInstallerInfoSync(id, path); }, std::move(callback));
}

void MiradClient::InstallGameAsync(QObject* context, const std::string& id, bool interactive,
                                   const std::string& installer,
                                   std::function<void(GameActionResult)> callback) {
  async::Run(context, [id, interactive, installer] { return InstallGameSync(id, interactive, installer); },
             std::move(callback));
}

void MiradClient::GetInstallProgressAsync(QObject* context, const std::string& id,
                                          std::function<void(InstallProgressResult)> callback) {
  async::Run(context, [id] { return GetInstallProgressSync(id); }, std::move(callback));
}

void MiradClient::RelocateLibraryAsync(QObject* context,
                                       std::function<void(RelocateLibraryResult)> callback) {
  RelocateLibraryJob(context, std::nullopt, std::move(callback));
}

void MiradClient::RelocateGamesAsync(QObject* context, const std::vector<std::string>& ids,
                                     std::function<void(RelocateLibraryResult)> callback) {
  RelocateLibraryJob(context, ids, std::move(callback));
}

void MiradClient::RelocateGameAsync(QObject* context, const std::string& id, const std::string& install_path,
                                    const std::string& data_dir, std::function<void(GameDetailResult)> callback) {
  json body = json::object();
  if (!install_path.empty()) body["install_path"] = install_path;
  if (!data_dir.empty()) body["data_dir"] = data_dir;
  RunJob<GameDetailResult>(
      context, "relocate",
      [id, body](const std::string& query) {
        return transport::PostJson("/v1/games/" + id + "/relocate" + query, body);
      },
      [id](GameDetailResult& result, const json& body) {
        if (!body.is_object()) throw std::runtime_error(transport::UnexpectedResponse("POST /v1/games/" + id + "/relocate"));
        result.game = mapping::ToGameDetail(body);
      },
      std::move(callback));
}

ArtworkResult MiradClient::GetArtworkBlocking(const std::string& id, const std::string& slot) {
  return GetArtworkSync(id, slot);
}

ArtworkResult MiradClient::GetTitleArtworkBlocking(const std::string& source, const std::string& ref) {
  return GetTitleArtworkSync(source, ref);
}

void MiradClient::DeleteGamesAsync(QObject* context, const std::vector<std::string>& ids, bool delete_files,
                                   bool delete_prefix, bool delete_metadata,
                                   std::function<void(DeleteGamesResult)> callback) {
  const json body = {{"ids", ids},
                     {"delete_files", delete_files},
                     {"delete_prefix", delete_prefix},
                     {"delete_metadata", delete_metadata}};
  RunJob<DeleteGamesResult>(
      context, "delete",
      [body](const std::string& query) { return transport::PostJson("/v1/games/delete" + query, body); },
      FillDeleteGames, std::move(callback));
}

bool MiradClient::ParseOpenConfig(const std::string& data) {
  const json entry = json::parse(data, nullptr, false);
  return entry.is_object() && entry.value("open_config", false);
}

bool MiradClient::ParseInstallEvent(const std::string& event_type, const std::string& data,
                                    InstallEvent* out) {
  constexpr std::string_view kPrefix = "game.install.";
  if (!event_type.starts_with(kPrefix)) return false;
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->state = event_type.substr(kPrefix.size());
  out->id = entry.value("id", std::string());
  out->error = mapping::ToApiError(entry);
  return !out->id.empty();
}

void MiradClient::BeginAmazonLoginAsync(QObject* context,
                                        std::function<void(LoginUrlResult)> callback) {
  async::Run(context, [] { return BeginAmazonLoginSync(); }, std::move(callback), async::Lane::Slow);
}

void MiradClient::GetLaunchersAsync(QObject* context, std::function<void(LaunchersResult)> callback) {
  async::Run(context, [] { return GetLaunchersSync(); }, std::move(callback));
}

void MiradClient::InstallLauncherAsync(QObject* context, const std::string& id,
                                       std::function<void(StoreActionResult)> callback) {
  async::Run(context, [id] { return InstallLauncherSync(id); }, std::move(callback));
}

void MiradClient::ImportLauncherAsync(QObject* context, const std::string& id,
                                      std::function<void(StoreImportResult)> callback) {
  RunJob<StoreImportResult>(
      context, "import",
      [id](const std::string& query) { return transport::Post("/v1/launchers/" + id + "/import" + query); },
      FillAddedUpdated<StoreImportResult>, std::move(callback));
}

void MiradClient::OpenLauncherAsync(QObject* context, const std::string& id,
                                    std::function<void(StoreActionResult)> callback) {
  async::Run(context, [id] { return OpenLauncherSync(id); }, std::move(callback));
}

bool MiradClient::ParseStoreEvent(const std::string& event_type, const std::string& data,
                                  StoreEvent* out) {
  // Each store's setup event has its own prefix; installs and downloads
  // share one each.
  static const std::pair<std::string_view, std::string_view> kSetupPrefixes[] = {
      {"epic.legendary.install.", "epic"},
      {"gog.setup.", "gog"},
      {"itch.setup.", "itch"},
      {"humble.setup.", "humble"},
      {"amazon.setup.", "amazon"},
      {"umu.setup.", "umu"},
      {"winetricks.setup.", "winetricks"},
  };
  std::string_view state;
  for (const auto& [prefix, source] : kSetupPrefixes) {
    if (event_type.starts_with(prefix)) {
      out->source = source;
      out->kind = "setup";
      state = std::string_view(event_type).substr(prefix.size());
    }
  }
  constexpr std::string_view kInstall = "library.install.";
  constexpr std::string_view kDownload = "humble.download.";
  constexpr std::string_view kLauncher = "launcher.install.";
  if (event_type.starts_with(kLauncher)) {
    out->kind = "setup";  // source is the launcher id, read below
    state = std::string_view(event_type).substr(kLauncher.size());
  } else if (event_type.starts_with(kInstall)) {
    out->kind = "install";
    state = std::string_view(event_type).substr(kInstall.size());
  } else if (event_type.starts_with(kDownload)) {
    out->source = "humble";
    out->kind = "download";
    state = std::string_view(event_type).substr(kDownload.size());
  }
  if (state.empty()) return false;

  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->state = std::string(state);
  if (out->kind == "install") {
    out->source = entry.value("source", std::string());
    out->ref = entry.value("ref", std::string());
    out->update = entry.value("update", false);
    if (out->state == "progress") {
      out->progress = entry.value("progress", 0.0);
      out->eta_seconds = static_cast<std::int64_t>(entry.value("eta", -1.0));
      out->bytes_per_second = entry.value("bps", -1.0);
    }
  } else if (out->kind == "download") {
    out->ref = entry.value("bundle_key", std::string());
    out->path = entry.value("path", std::string());
    out->downloaded = entry.value("downloaded", true);
  } else if (event_type.starts_with(kLauncher)) {
    out->source = entry.value("id", std::string());
  }
  out->error = mapping::ToApiError(entry);
  return true;
}

bool MiradClient::ParseRunnerDownload(const std::string& event_type, const std::string& data,
                                      RunnerDownloadEvent* out) {
  constexpr std::string_view kPrefix = "runners.download.";
  if (!event_type.starts_with(kPrefix)) return false;

  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->state = event_type.substr(kPrefix.size());
  out->kind = entry.value("kind", std::string());
  out->tag = entry.value("tag", std::string());
  out->name = entry.value("name", std::string());
  out->label = entry.value("label", out->name);
  out->source = entry.value("source", std::string());
  out->replaced = entry.value("replaced", std::string());
  out->error = mapping::ToApiError(entry);
  return true;
}

bool MiradClient::ParseTricksEvent(const std::string& event_type, const std::string& data,
                                   TricksEvent* out) {
  constexpr std::string_view kPrefix = "tricks.";
  if (!event_type.starts_with(kPrefix)) return false;

  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->state = event_type.substr(kPrefix.size());
  out->id = entry.value("id", std::string());
  out->verb = entry.value("verb", std::string());
  out->error = mapping::ToApiError(entry);
  return true;
}

}  // namespace mira_gui
