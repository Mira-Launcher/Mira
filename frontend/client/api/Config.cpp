#include "Config.h"

#include <cctype>
#include <chrono>
#include <json.hpp>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "../Async.h"
#include "../Jobs.h"
#include "../JsonMapping.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
namespace {

using nlohmann::json;

HealthStatus GetHealthSync() {
  HealthStatus status;
  const transport::Reply reply = transport::Get("/v1/health");
  status.reachable = reply.ok;
  if (!reply.ok) {
    status.detail = reply.error.message;
    return status;
  }
  const bool object = reply.body.is_object();
  const json detail = object ? reply.body.value("status", json("ok")) : json("ok");
  status.detail = detail.is_string() ? detail.get<std::string>() : "ok";
  if (object && reply.body.contains("api") && reply.body["api"].is_number_integer()) {
    status.api = reply.body["api"].get<int>();
  }
  return status;
}

void FillConfigSchema(ConfigSchemaResult& result, const json& body) {
  for (const json& entry : body) {
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
}

ConfigSchemaResult GetConfigSchemaSync() {
  return ReadReply<ConfigSchemaResult>(transport::Get("/v1/config/schema"), "GET /v1/config/schema",
                                       Shape::Array, FillConfigSchema);
}

ConfigResult GetConfigSync() {
  return ReadReply<ConfigResult>(transport::Get("/v1/config"), "GET /v1/config", Shape::Object,
                                 [](ConfigResult& result, const json& body) {
                                   mapping::FlattenConfig(body, "", result.values);
                                 });
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

void FillFrontendPrefs(FrontendPrefsResult& result, const json& body);

FrontendPrefsResult GetFrontendPrefsSync() {
  return ReadReply<FrontendPrefsResult>(transport::Get("/v1/config"), "GET /v1/config",
                                        Shape::Object, FillFrontendPrefs);
}

void FillFrontendPrefs(FrontendPrefsResult& result, const json& body) {
  // Frontend falls back to defaults rather than refusing to start on absense
  // or wrong kind after a hand-edit.
  const json table = body.value("frontend", json::object());
  if (!table.is_object()) return;

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
  read_bool("double_click_play", result.prefs.double_click_play);
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
  read_bool("library_continue_apps", result.prefs.library_continue_apps);
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
  const auto read_strings = [&table](const char* key,
                                     std::optional<std::vector<std::string>>& out) {
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
  if (prefs.double_click_play) table["double_click_play"] = *prefs.double_click_play;
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
  if (prefs.library_continue_apps) table["library_continue_apps"] = *prefs.library_continue_apps;
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

GameModeStatusResult GetGameModeStatusSync() {
  return ReadReply<GameModeStatusResult>(
      transport::Get("/v1/gamemode/status"), "GET /v1/gamemode/status", Shape::Object,
      [](GameModeStatusResult& result, const json& body) {
        result.installed = body.value("installed", false);
        result.daemon_running = body.value("daemon_running", false);
      });
}

}  // namespace

std::string ResolveSocketPath() {
  return transport::SocketPath();
}

void CheckHealthAsync(QObject* context, std::function<void(HealthStatus)> callback) {
  async::Run(context, [] { return GetHealthSync(); }, std::move(callback));
}

void GetConfigSchemaAsync(QObject* context, std::function<void(ConfigSchemaResult)> callback) {
  async::Run(context, [] { return GetConfigSchemaSync(); }, std::move(callback));
}

void GetConfigAsync(QObject* context, std::function<void(ConfigResult)> callback) {
  async::Run(context, [] { return GetConfigSync(); }, std::move(callback));
}

void PatchConfigAsync(QObject* context, const std::vector<ConfigEdit>& edits,
                      std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [edits] { return PatchConfigSync(edits); }, std::move(callback));
}

void ResetConfigKeyAsync(QObject* context, const std::string& key,
                         std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [key] { return ResetConfigKeySync(key); }, std::move(callback));
}

void GetFrontendPrefsAsync(QObject* context, std::function<void(FrontendPrefsResult)> callback) {
  async::Run(context, [] { return GetFrontendPrefsSync(); }, std::move(callback));
}

void SaveFrontendPrefsAsync(QObject* context, const FrontendPrefs& prefs,
                            std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [prefs] { return SaveFrontendPrefsSync(prefs); }, std::move(callback));
}

PatchConfigResult SaveFrontendPrefsBlocking(const FrontendPrefs& prefs) {
  return SaveFrontendPrefsSync(prefs);
}

FrontendPrefsResult GetFrontendPrefsBlocking() {
  return GetFrontendPrefsSync();
}

void GetGameModeStatusAsync(QObject* context, std::function<void(GameModeStatusResult)> callback) {
  async::Run(context, [] { return GetGameModeStatusSync(); }, std::move(callback));
}

}  // namespace mira_gui::api
