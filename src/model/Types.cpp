#include "model/Types.h"

#include <chrono>

namespace mira::model {
namespace {
using nlohmann::json;
}

std::string_view ToString(Platform platform) {
  switch (platform) {
    case Platform::Windows: return "windows";
    case Platform::Native:  return "native";
    case Platform::Unknown: return "unknown";
  }
  return "unknown";
}

std::string_view ToString(GameStatus status) {
  switch (status) {
    case GameStatus::SettingUp:    return "setting_up";
    case GameStatus::Ready:        return "ready";
    case GameStatus::Broken:       return "broken";
    case GameStatus::Missing:      return "missing";
    case GameStatus::NeedsInstall: return "needs_install";
  }
  return "broken";
}

std::string_view ToString(NotifyLevel level) {
  switch (level) {
    case NotifyLevel::Success: return "success";
    case NotifyLevel::Warning: return "warning";
    case NotifyLevel::Error:   return "error";
    case NotifyLevel::Info:    break;
  }
  return "info";
}

Platform PlatformFromString(std::string_view text) {
  if (text == "windows") return Platform::Windows;
  if (text == "native") return Platform::Native;
  return Platform::Unknown;
}

GameStatus GameStatusFromString(std::string_view text) {
  if (text == "setting_up") return GameStatus::SettingUp;
  if (text == "ready") return GameStatus::Ready;
  if (text == "missing") return GameStatus::Missing;
  if (text == "needs_install") return GameStatus::NeedsInstall;
  return GameStatus::Broken;
}

std::int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

json ToJson(const Candidate& candidate) {
  return {
      {"rel_path", candidate.rel_path},
      {"kind", ToString(candidate.kind)},
      {"score", candidate.score},
      {"chosen", candidate.chosen},
      {"is_installer", candidate.is_installer},
  };
}

json ToJson(const Game& game) {
  json out = {
      {"id", game.id},
      {"install_path", game.install_path},
      {"name", game.name},
      {"status", ToString(game.status)},
      {"confidence", game.confidence},
      {"reviewed", game.reviewed},
      {"platform", ToString(game.platform)},
      {"source", game.source},
      {"exe_path", game.exe_path},
      {"args", game.args},
      {"working_dir", game.working_dir},
      {"runner_ref", game.runner_ref},
      {"source_ref", game.source_ref},
      {"data_dir", game.data_dir},
      {"installer_dir", game.installer_dir},
      {"runner_config", game.runner_config},
      {"overrides", game.overrides},
      {"last_error", game.last_error},
      {"created_at", game.created_at},
      {"updated_at", game.updated_at},
      {"play_seconds", game.play_seconds},
      {"last_session_at", game.last_session_at},
      {"env", game.env},
  };
  out["last_played_at"] = game.last_played_at ? json(*game.last_played_at) : json(nullptr);

  json candidates = json::array();
  for (const Candidate& candidate : game.candidates) candidates.push_back(ToJson(candidate));
  out["candidates"] = std::move(candidates);
  out["tags"] = game.tags;
  return out;
}

json ToJson(const RunnerBuild& runner) {
  return {
      {"kind", runner.kind},
      {"name", runner.name},
      {"path", runner.path},
      {"version", runner.version},
      {"reference", runner.Reference()},
  };
}

json ToJson(const Event& event) {
  return {{"id", event.id}, {"ts", event.ts}, {"type", event.type}, {"payload", event.payload}};
}

Game GameFromJson(const json& document) {
  Game game;
  game.id = document.value("id", std::string());
  game.install_path = document.value("install_path", std::string());
  game.name = document.value("name", std::string());
  game.status = GameStatusFromString(document.value("status", "broken"));
  game.confidence = document.value("confidence", 0.0);
  game.reviewed = document.value("reviewed", false);
  game.platform = PlatformFromString(document.value("platform", "unknown"));
  // "scan", not empty: an old games.toml written before this field existed
  // was necessarily a folder scan: Steam/Lutris imports are new enough
  // that every row they ever wrote already carries their own source.
  game.source = document.value("source", "scan");
  game.exe_path = document.value("exe_path", std::string());
  game.args = document.value("args", std::string());
  game.working_dir = document.value("working_dir", std::string());
  game.runner_ref = document.value("runner_ref", std::string());
  game.source_ref = document.value("source_ref", std::string());
  game.data_dir = document.value("data_dir", std::string());
  game.installer_dir = document.value("installer_dir", std::string());
  if (document.contains("runner_config") && document["runner_config"].is_object()) {
    game.runner_config = document["runner_config"];
  }
  if (document.contains("overrides") && document["overrides"].is_object()) {
    game.overrides = document["overrides"];
  }
  game.last_error = document.value("last_error", std::string());
  game.created_at = document.value("created_at", std::int64_t{0});
  game.updated_at = document.value("updated_at", std::int64_t{0});
  if (document.contains("last_played_at") && document["last_played_at"].is_number()) {
    game.last_played_at = document["last_played_at"].get<std::int64_t>();
  }
  game.play_seconds = document.value("play_seconds", std::int64_t{0});
  game.last_session_at = document.value("last_session_at", std::int64_t{0});

  if (document.contains("env") && document["env"].is_object()) {
    for (const auto& [key, value] : document["env"].items()) {
      if (value.is_string()) game.env[key] = value.get<std::string>();
    }
  }

  if (document.contains("tags") && document["tags"].is_array()) {
    for (const json& tag : document["tags"]) {
      if (tag.is_string()) game.tags.push_back(tag.get<std::string>());
    }
  }

  if (document.contains("candidates") && document["candidates"].is_array()) {
    for (const json& entry : document["candidates"]) {
      Candidate candidate;
      candidate.rel_path = entry.value("rel_path", std::string());
      candidate.kind = PlatformFromString(entry.value("kind", "unknown"));
      candidate.score = entry.value("score", 0.0);
      candidate.chosen = entry.value("chosen", false);
      candidate.is_installer = entry.value("is_installer", false);
      game.candidates.push_back(std::move(candidate));
    }
  }
  return game;
}

}  // namespace mira::model
