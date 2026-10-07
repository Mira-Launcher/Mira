#include "JsonMapping.h"

#include <cstdint>
#include <set>
#include <utility>
#include <string>

namespace mira_gui::mapping {

using nlohmann::json;

namespace {
// Tags array for a game: free-form, with "hidden" treated specially.
// Read the same way everywhere a game arrives from mirad, and carried
// under the same key in the GETs and the SSE payloads.
std::vector<std::string> ReadTags(const json& entry) {
  std::vector<std::string> tags;
  for (const json& tag : entry.value("tags", json::array())) {
    if (tag.is_string()) tags.push_back(tag.get<std::string>());
  }
  return tags;
}

// `sort_root` and `folder_tags`, each null when it doesn't apply.
void ReadSorting(const json& entry, std::string& sort_root, std::optional<std::vector<std::string>>& folder_tags) {
  sort_root = entry.contains("sort_root") && entry["sort_root"].is_string() ? entry["sort_root"].get<std::string>()
                                                                             : std::string();
  folder_tags.reset();
  if (entry.contains("folder_tags") && entry["folder_tags"].is_array()) {
    folder_tags = ReadTags({{"tags", entry["folder_tags"]}});
  }
}
}  // namespace

ApiError ToApiError(const json& error) {
  ApiError out;
  out.message = error.contains("message") ? Str(error, "message") : Str(error, "error");
  out.code = Str(error, "code");
  out.hint = Str(error, "hint");
  if (error.contains("fix") && error["fix"].is_object()) {
    const json& fix = error["fix"];
    out.fix = {Str(fix, "kind"), Str(fix, "target"), Str(fix, "step")};
  }
  return out;
}

GameSummary ToGameSummary(const json& entry) {
  GameSummary game;
  game.id = entry.value("id", std::string());
  game.name = entry.value("name", std::string());
  game.status = entry.value("status", std::string());
  game.platform = entry.value("platform", std::string());
  game.runner_ref = entry.value("runner_ref", std::string());
  game.last_error = entry.value("last_error", std::string());
  game.install_path = entry.value("install_path", std::string());
  game.needs_check = entry.value("needs_check", false);
  if (entry.contains("last_played_at") && entry["last_played_at"].is_number()) {
    game.last_played_at = entry["last_played_at"].get<std::int64_t>();
  }
  game.play_seconds = entry.value("play_seconds", std::int64_t{0});
  game.tags = ReadTags(entry);
  game.source = entry.value("source", std::string("scan"));
  game.running = entry.value("running", false);
  game.art = ToArtVersions(entry);
  ReadSorting(entry, game.sort_root, game.folder_tags);
  return game;
}

std::optional<ArtVersions> ToArtVersions(const json& entry) {
  if (!entry.is_object() || !entry.contains("art") || !entry["art"].is_object()) return std::nullopt;
  ArtVersions art;
  for (const auto& [slot, version] : entry["art"].items()) {
    if (version.is_string()) art[slot] = version.get<std::string>();
  }
  return art;
}

GameDetail ToGameDetail(const json& entry) {
  GameDetail game;
  game.id = entry.value("id", std::string());
  game.name = entry.value("name", std::string());
  game.status = entry.value("status", std::string());
  game.platform = entry.value("platform", std::string());
  game.source = entry.value("source", std::string("scan"));
  game.install_path = entry.value("install_path", std::string());
  game.exe_path = entry.value("exe_path", std::string());
  game.args = entry.value("args", std::string());
  game.working_dir = entry.value("working_dir", std::string());
  game.runner_ref = entry.value("runner_ref", std::string());
  game.data_dir = entry.value("data_dir", std::string());
  game.last_error = entry.value("last_error", std::string());
  game.needs_check = entry.value("needs_check", false);
  if (entry.contains("last_played_at") && entry["last_played_at"].is_number()) {
    game.last_played_at = entry["last_played_at"].get<std::int64_t>();
  }
  game.play_seconds = entry.value("play_seconds", std::int64_t{0});
  game.runner_config_json = entry.value("runner_config", json::object()).dump(2);
  game.env_json = entry.value("env", json::object()).dump(2);
  game.default_runner = entry.value("default_runner", std::string());
  game.tags = ReadTags(entry);

  for (const json& candidate : entry.value("candidates", json::array())) {
    GameDetail::Candidate c;
    c.rel_path = candidate.value("rel_path", std::string());
    c.kind = candidate.value("kind", std::string());
    c.score = candidate.value("score", 0.0);
    c.chosen = candidate.value("chosen", false);
    c.is_installer = candidate.value("is_installer", false);
    game.candidates.push_back(std::move(c));
  }
  return game;
}

std::string ToDisplayString(const json& value) {
  if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
  if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
  if (value.is_number()) return value.dump();
  if (value.is_string()) return value.get<std::string>();
  return value.dump();  // an array stays JSON, so an item may hold a comma
}

void FlattenConfig(const json& node, const std::string& prefix,
                   std::map<std::string, std::string>& out) {
  if (!node.is_object()) {
    out[prefix] = ToDisplayString(node);
    return;
  }
  for (const auto& [key, value] : node.items()) {
    if (prefix.empty() && key == "frontend") continue;  // opaque, not part of the schema
    const std::string dotted = prefix.empty() ? key : prefix + "." + key;
    if (value.is_object()) {
      FlattenConfig(value, dotted, out);
    } else {
      out[dotted] = ToDisplayString(value);
    }
  }
}

json MergePatchBetween(const json& before, const json& after) {
  json patch = json::object();
  if (before.is_object()) {
    for (const auto& [key, value] : before.items()) {
      if (!after.is_object() || !after.contains(key)) patch[key] = nullptr;
    }
  }
  if (after.is_object()) {
    for (const auto& [key, value] : after.items()) {
      if (!before.is_object() || !before.contains(key) || before[key] != value) patch[key] = value;
    }
  }
  return patch;
}

std::vector<std::string> ParseListText(const std::string& text) {
  std::vector<std::string> items;
  const json parsed = json::parse(text, nullptr, false);
  if (!parsed.is_array()) {
    if (!text.empty()) items.push_back(text);
    return items;
  }
  for (const json& item : parsed) {
    std::string value = item.is_string() ? item.get<std::string>() : item.dump();
    if (!value.empty()) items.push_back(std::move(value));
  }
  return items;
}

std::string ListText(const std::vector<std::string>& items) { return json(items).dump(); }

json TypedValueFromText(const std::string& type, const std::string& value) {
  if (type == "a boolean") return value == "true";
  if (type == "an integer") {
    try {
      return static_cast<std::int64_t>(std::stoll(value));
    } catch (...) {
      return value;
    }
  }
  if (type == "a number") {
    try {
      return std::stod(value);
    } catch (...) {
      return value;
    }
  }
  if (type == "an array of strings") return ParseListText(value);
  return value;
}

void AssignDottedKey(json& document, const std::string& dotted_key, const json& value) {
  json* cursor = &document;
  size_t start = 0;
  while (true) {
    const size_t dot = dotted_key.find('.', start);
    const std::string segment =
        dotted_key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (dot == std::string::npos) {
      (*cursor)[segment] = value;
      return;
    }
    cursor = &(*cursor)[segment];
    start = dot + 1;
  }
}

ArtCandidate ToArtCandidate(const json& item) {
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

std::vector<GameFailure> ToGameFailures(const json& reply, const char* key) {
  std::vector<GameFailure> out;
  if (!reply.is_object() || !reply.contains(key) || !reply[key].is_array()) return out;
  for (const json& entry : reply[key]) {
    if (!entry.is_object()) continue;
    out.push_back({entry.value("id", std::string()), ToApiError(entry.value("error", json::object()))});
  }
  return out;
}

}  // namespace mira_gui::mapping
