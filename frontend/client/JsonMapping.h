#pragma once

#include <json.hpp>

#include <map>
#include <string>
#include <vector>

#include "Types.h"

// Conversions between mirad's JSON and the structs in Types.h.
// Not in the endpoints because these are meanings, not I/O.
namespace mira_gui::mapping {

// Typed reads that never throw: a missing, null or wrong-typed key gives the fallback.
inline std::string Str(const nlohmann::json& object, const char* key, std::string fallback = "") {
  if (!object.is_object()) return fallback;
  const auto it = object.find(key);
  return (it != object.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

inline int Int(const nlohmann::json& object, const char* key, int fallback = 0) {
  if (!object.is_object()) return fallback;
  const auto it = object.find(key);
  return (it != object.end() && it->is_number_integer()) ? it->get<int>() : fallback;
}

// An error object: {"code", "message", "hint"?, "fix"?} as in mirad's error
// envelope, or the same fields on a failure event (with "error" for the message).
ApiError ToApiError(const nlohmann::json& error);

GameSummary ToGameSummary(const nlohmann::json& entry);
// A record's or art event's `art`; unset when it has none.
std::optional<ArtVersions> ToArtVersions(const nlohmann::json& entry);
GameDetail ToGameDetail(const nlohmann::json& entry);

// A settings value as text: a string as is, anything else (an array included) as JSON.
std::string ToDisplayString(const nlohmann::json& value);

// Flattens GET /v1/config's nested document to the dotted keys Schema
// entries use, skipping the opaque `frontend` table
void FlattenConfig(const nlohmann::json& node, const std::string& prefix,
                   std::map<std::string, std::string>& out);

// The flat JSON merge patch that turns object `before` into `after`: changed keys, and null for removed ones.
nlohmann::json MergePatchBetween(const nlohmann::json& before, const nlohmann::json& after);

// A list setting's text (ToDisplayString of an array) as its items, and back.
std::vector<std::string> ParseListText(const std::string& text);
std::string ListText(const std::vector<std::string>& items);

// Converts edited display text back to the JSON kind its schema `type`
// calls for. Falls back to sending raw text for a malformed number.
nlohmann::json TypedValueFromText(const std::string& type, const std::string& value);

// Expands a dotted key into nested objects inside `document`, assigning
// `value` at the leaf, which is the shape PATCH /v1/config takes.
void AssignDottedKey(nlohmann::json& document, const std::string& dotted_key,
                     const nlohmann::json& value);

}  // namespace mira_gui::mapping
