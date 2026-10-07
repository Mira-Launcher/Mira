#include "library/GamePatch.h"

#include <filesystem>
#include <format>

#include "config/Resolver.h"
#include "config/Schema.h"
#include "library/FolderTags.h"

namespace mira::library {
using nlohmann::json;

std::string StoredExePath(const std::string& install_path, const std::string& exe_path) {
  if (install_path.empty() || exe_path.empty() || std::filesystem::path(exe_path).is_absolute()) return exe_path;
  const std::filesystem::path full = (std::filesystem::path(install_path) / exe_path).lexically_normal();
  const std::filesystem::path inside = full.lexically_relative(std::filesystem::path(install_path).lexically_normal());
  if (inside.empty() || *inside.begin() == "..") return full.string();
  return exe_path;
}

bool IsSettablePlatform(const json& value) {
  return value.is_string() && model::PlatformFromString(value.get<std::string>()) != model::Platform::Unknown;
}

model::Game ParseGamePatch(const model::Game& base, const json& patch) {
  model::Game game = base;
  if (patch.contains("name") && patch["name"].is_string()) game.name = patch["name"];
  if (patch.contains("exe_path") && patch["exe_path"].is_string()) {
    game.exe_path = StoredExePath(game.install_path, patch["exe_path"]);
  }
  if (patch.contains("args") && patch["args"].is_string()) game.args = patch["args"];
  if (patch.contains("platform") && patch["platform"].is_string()) {
    game.platform = model::PlatformFromString(patch["platform"].get<std::string>());
  }
  if (patch.contains("working_dir") && patch["working_dir"].is_string()) {
    game.working_dir = patch["working_dir"];
  }
  if (patch.contains("runner_ref") && patch["runner_ref"].is_string()) {
    game.runner_ref = patch["runner_ref"];
  }
  if (patch.contains("data_dir") && patch["data_dir"].is_string()) {
    game.data_dir = patch["data_dir"];
  }
  if (patch.contains("runner_config") && patch["runner_config"].is_object()) {
    game.runner_config.merge_patch(patch["runner_config"]);
  }
  // Replaced, not merged; {"tags": []} clears them.
  if (patch.contains("tags") && patch["tags"].is_array()) {
    game.tags.clear();
    for (const auto& tag : patch["tags"]) {
      if (tag.is_string()) game.tags.push_back(tag.get<std::string>());
    }
  }
  // "" goes back to tags.folders' order.
  if (patch.contains("folder_tag") && patch["folder_tag"].is_string())
    game.folder_tag = patch["folder_tag"];
  DropStalePick(game);
  // "env": null clears every entry; {"K": null} removes just K.
  if (patch.contains("env") && patch["env"].is_null()) {
    game.env.clear();
  } else if (patch.contains("env") && patch["env"].is_object()) {
    for (const auto& [key, value] : patch["env"].items()) {
      if (value.is_null()) {
        game.env.erase(key);
      } else if (value.is_string()) {
        game.env[key] = value.get<std::string>();
      }
    }
  }
  // A correction counts as the human having looked; a patch that changed nothing doesn't.
  if (model::ToJson(game) != model::ToJson(base)) game.reviewed = true;
  if (patch.contains("reviewed") && patch["reviewed"].is_boolean()) game.reviewed = patch["reviewed"];
  return game;
}

// The first wrong-typed field of a game patch, named, so a bad body is a 400 rather than silently ignored.
std::optional<std::string> GamePatchProblem(const json& patch) {
  if (!patch.is_object()) return "expected a JSON object";
  for (const char* key :
       {"name", "exe_path", "args", "working_dir", "runner_ref", "data_dir", "folder_tag"}) {
    if (patch.contains(key) && !patch[key].is_string()) return std::format("\"{}\" must be a string", key);
  }
  if (patch.contains("platform") && !IsSettablePlatform(patch["platform"])) {
    return "\"platform\" must be \"windows\" or \"native\"";
  }
  if (patch.contains("tags") && !patch["tags"].is_array()) return "\"tags\" must be an array";
  if (patch.contains("runner_config") && !patch["runner_config"].is_object()) return "\"runner_config\" must be an object";
  if (patch.contains("env") && !patch["env"].is_object() && !patch["env"].is_null()) return "\"env\" must be an object or null";
  if (patch.contains("reviewed") && !patch["reviewed"].is_boolean()) return "\"reviewed\" must be true or false";
  return std::nullopt;
}

// Applies a flat {"dotted.key": value} overrides patch; null removes an override.
void ApplyOverridesPatch(model::Game& game, const json& patch) {
  for (const auto& [key, value] : patch.items()) {
    if (value.is_null()) {
      game.overrides.erase(key);
    } else {
      game.overrides[key] = value;
    }
  }
}

// Checked first so a bad key rejects the whole patch, like Config::Patch.
std::optional<std::string> ValidateOverridesPatch(const json& patch) {
  for (const auto& [key, value] : patch.items()) {
    if (value.is_null()) continue;  // removal; nothing to validate
    if (!config::Resolver::IsOverridable(key)) {
      return std::format("\"{}\" cannot be overridden per game", key);
    }
    if (auto problem = config::Schema::Instance().Validate(key, value)) {
      return std::format("{}: {}", key, *problem);
    }
  }
  return std::nullopt;
}

}  // namespace mira::library
