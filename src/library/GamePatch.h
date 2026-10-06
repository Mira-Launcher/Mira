#pragma once

#include <optional>
#include <string>

#include <json.hpp>

#include "model/Types.h"

namespace mira::library {

// `exe_path` as a game stores it: a relative path that leaves `install_path` becomes absolute,
// since a "../" path would point somewhere else once the folder moves.
std::string StoredExePath(const std::string& install_path, const std::string& exe_path);

// "windows" or "native": the platforms a client may give a game. "unknown" is only ever detected.
bool IsSettablePlatform(const nlohmann::json& value);

// `base` with the fields of a PATCH /v1/games/{id} body applied.
model::Game ParseGamePatch(const model::Game& base, const nlohmann::json& patch);

// The first wrong-typed field of a game patch, named, so a bad body is a 400 rather than silently ignored.
std::optional<std::string> GamePatchProblem(const nlohmann::json& patch);

// Applies a flat {"dotted.key": value} overrides patch; null removes an override.
void ApplyOverridesPatch(model::Game& game, const nlohmann::json& patch);

// Checked first so a bad key rejects the whole patch, like Config::Patch.
std::optional<std::string> ValidateOverridesPatch(const nlohmann::json& patch);

}  // namespace mira::library
