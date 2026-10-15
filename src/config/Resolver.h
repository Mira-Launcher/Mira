#pragma once

#include <json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "config/Config.h"

namespace mira::config {

// Which layer supplied a value. Reported alongside every resolved setting:
// deep configurability is confusing precisely when you cannot tell why a value
// is what it is, so the frontend can always show "inherited" versus "overridden
// here" and offer a reset.
enum class Layer { Default, ConfigFile, Game };

std::string_view ToString(Layer layer);

struct Resolved {
  nlohmann::json value;
  Layer layer;
};

// Resolves settings through default < config file < per-game override.
// Overrides are a flat JSON object of dotted keys, stored on the game row.
class Resolver {
 public:
  Resolver(const Config& config, nlohmann::json game_overrides);

  Resolved Resolve(std::string_view key) const;
  nlohmann::json Value(std::string_view key) const { return Resolve(key).value; }

  bool GetBool(std::string_view key) const;
  std::int64_t GetInt(std::string_view key) const;
  double GetDouble(std::string_view key) const;
  std::string GetString(std::string_view key) const;
  std::vector<std::string> GetStringArray(std::string_view key) const;

  // Every declared key with its resolved value and originating layer.
  nlohmann::json EffectiveDocument() const;

  static bool IsOverridable(std::string_view key);

 private:
  const Config& config_;
  nlohmann::json overrides_;
};

}  // namespace mira::config
