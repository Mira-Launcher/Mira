#pragma once

#include <optional>
#include <string>

#include <json.hpp>

#include "model/Types.h"

namespace mira::library {

// `base` with the fields of a PATCH /v1/games/{id} body applied.
model::Game ParseGamePatch(const model::Game& base, const nlohmann::json& patch);

// The first wrong-typed field of a game patch, named, so a bad body is a 400 rather than silently ignored.
std::optional<std::string> GamePatchProblem(const nlohmann::json& patch);

// Applies a flat {"dotted.key": value} overrides patch; null removes an override.
void ApplyOverridesPatch(model::Game& game, const nlohmann::json& patch);

// Checked first so a bad key rejects the whole patch, like Config::Patch.
std::optional<std::string> ValidateOverridesPatch(const nlohmann::json& patch);

}  // namespace mira::library
