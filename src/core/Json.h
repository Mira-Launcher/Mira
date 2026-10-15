#pragma once

#include <cstdint>
#include <json.hpp>
#include <string>

namespace mira::core {

// Scans `text` from the end for the last line that parses as complete
// JSON on its own, skipping over any subprocess log noise around it (a
// tool's own log lines never parse as JSON, however many stray
// '{'/'['/']'/'}' characters they contain; legendary's "[Core] ..." and
// "[cli] ..." lines fool a naive "find the first bracket" scan). Shared by every source that mixes
// its --json payload with its own stderr/stdout log noise (Legendary,
// gogdl, and likely butler too).
//
// Returns a discarded json (is_discarded() true) if nothing parses --
// never json()'s ordinary null, which a caller checking only
// is_discarded() would misread as "found nothing, but that's a valid
// result" rather than the parse failure it actually is.
nlohmann::json ParseJsonTail(const std::string& text);

// Typed reads that never throw: a missing, null or wrong-typed key gives the fallback.
inline std::string JsonString(const nlohmann::json& j, const char* key, std::string fallback = "") {
  if (!j.is_object()) return fallback;
  const auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

inline int64_t JsonInt(const nlohmann::json& j, const char* key, int64_t fallback = 0) {
  if (!j.is_object()) return fallback;
  const auto it = j.find(key);
  return (it != j.end() && it->is_number_integer()) ? it->get<int64_t>() : fallback;
}

inline bool JsonBool(const nlohmann::json& j, const char* key, bool fallback = false) {
  if (!j.is_object()) return fallback;
  const auto it = j.find(key);
  return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

inline double JsonDouble(const nlohmann::json& j, const char* key, double fallback = 0) {
  if (!j.is_object()) return fallback;
  const auto it = j.find(key);
  return (it != j.end() && it->is_number()) ? it->get<double>() : fallback;
}

// The key's array or object, or an empty one.
inline nlohmann::json JsonArray(const nlohmann::json& j, const char* key) {
  if (!j.is_object()) return nlohmann::json::array();
  const auto it = j.find(key);
  return (it != j.end() && it->is_array()) ? *it : nlohmann::json::array();
}

inline nlohmann::json JsonObject(const nlohmann::json& j, const char* key) {
  if (!j.is_object()) return nlohmann::json::object();
  const auto it = j.find(key);
  return (it != j.end() && it->is_object()) ? *it : nlohmann::json::object();
}

}  // namespace mira::core
