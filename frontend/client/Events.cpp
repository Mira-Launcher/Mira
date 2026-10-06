#include "Events.h"

#include <json.hpp>

#include "JsonMapping.h"

namespace mira_gui::events {

using nlohmann::json;

bool ParseGameSummary(const std::string& data, GameSummary* out) {
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

bool ParseGameSummaries(const std::string& data, std::vector<GameSummary>* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object() || !entry.contains("games") ||
      !entry["games"].is_array()) {
    return false;
  }
  out->clear();
  for (const json& game : entry["games"]) {
    if (mapping::Str(game, "id") != "") out->push_back(mapping::ToGameSummary(game));
  }
  return true;
}

bool ParseGameState(const std::string& data, GameStateEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->id = entry.value("id", std::string());
  out->state = entry.value("state", std::string());
  out->played_seconds = entry.value("played_seconds", std::int64_t{0});
  out->error = mapping::ToApiError(entry);
  return !out->id.empty();
}

bool ParseGameLaunched(const std::string& data, GameLaunchedEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->id = entry.value("id", std::string());
  if (out->id.empty()) return false;
  out->tracked = entry.value("tracked", false);
  return true;
}

bool ParseInstallDetected(const std::string& data, InstallDetectedEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->id = mapping::Str(entry, "id");
  out->install_path = mapping::Str(entry, "install_path");
  out->exe_path = mapping::Str(entry, "exe_path");
  return !out->id.empty() && !out->install_path.empty();
}

std::string ParseRemovedId(const std::string& data) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return {};
  return entry.value("id", std::string());
}

std::vector<std::string> ParseRemovedIds(const std::string& data) {
  const json entry = json::parse(data, nullptr, false);
  std::vector<std::string> ids;
  if (entry.is_discarded() || !entry.is_object() || !entry.contains("ids") ||
      !entry["ids"].is_array())
    return ids;
  for (const json& id : entry["ids"]) {
    if (id.is_string() && !id.get<std::string>().empty()) ids.push_back(id.get<std::string>());
  }
  return ids;
}

bool ParseMetadataEvent(const std::string& data, MetadataEvent* out) {
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

bool ParseArtworkSelectEvent(const std::string& data, ArtworkSelectEvent* out) {
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

bool ParseArtCandidatesEvent(const std::string& data, ArtCandidatesEvent* out) {
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
    for (const json& item : payload["candidates"])
      out->candidates.push_back(mapping::ToArtCandidate(item));
  }
  return true;
}

bool ParseArtThumbsEvent(const std::string& data, ArtThumbsEvent* out) {
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

bool ParseNotification(const std::string& data, NotificationEvent* out) {
  const json payload = json::parse(data, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) return false;
  out->message = payload.value("message", std::string());
  if (out->message.empty()) return false;
  out->level = payload.value("level", std::string("info"));
  return true;
}

bool ParseTitleArtworkEvent(const std::string& event_type, const std::string& data,
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

bool ParseOpenConfig(const std::string& data) {
  const json entry = json::parse(data, nullptr, false);
  return entry.is_object() && entry.value("open_config", false);
}

bool ParseAutoInstall(const std::string& data) {
  const json entry = json::parse(data, nullptr, false);
  return entry.is_object() && entry.value("auto_install", false);
}

bool ParseInstallerLeftover(const std::string& data, InstallerLeftoverEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (!entry.is_object()) return false;
  out->id = entry.value("id", std::string());
  out->installer_dir = entry.value("installer_dir", std::string());
  out->bytes = entry.value("bytes", std::int64_t{0});
  return !out->id.empty() && !out->installer_dir.empty();
}

bool ParseInstallEvent(const std::string& event_type, const std::string& data, InstallEvent* out) {
  constexpr std::string_view kPrefix = "game.install.";
  if (!event_type.starts_with(kPrefix)) return false;
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->state = event_type.substr(kPrefix.size());
  out->id = entry.value("id", std::string());
  out->error = mapping::ToApiError(entry);
  return !out->id.empty();
}

bool ParseStoreEvent(const std::string& event_type, const std::string& data, StoreEvent* out) {
  // Each runner tool's setup event has its own prefix. (A store's setup is a job.)
  static const std::pair<std::string_view, std::string_view> kSetupPrefixes[] = {
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
  constexpr std::string_view kLauncher = "launcher.install.";
  if (event_type.starts_with(kLauncher)) {
    out->kind = "setup";  // source is the launcher id, read below
    state = std::string_view(event_type).substr(kLauncher.size());
  } else if (event_type.starts_with(kInstall)) {
    out->kind = "install";
    state = std::string_view(event_type).substr(kInstall.size());
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
  } else if (event_type.starts_with(kLauncher)) {
    out->source = entry.value("id", std::string());
    if (out->state == "progress") out->progress = entry.value("progress", 0.0);
  }
  out->error = mapping::ToApiError(entry);
  return true;
}

bool ParseRunnerDownload(const std::string& event_type, const std::string& data,
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
  out->progress = entry.value("progress", -1.0);
  out->error = mapping::ToApiError(entry);
  return true;
}

bool ParseTricksEvent(const std::string& event_type, const std::string& data, TricksEvent* out) {
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

}  // namespace mira_gui::events
