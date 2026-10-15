#include "config/Resolver.h"

#include "config/Schema.h"

namespace mira::config {

using nlohmann::json;

std::string_view ToString(Layer layer) {
  switch (layer) {
    case Layer::Default:
      return "default";
    case Layer::ConfigFile:
      return "config";
    case Layer::Game:
      return "game";
  }
  return "default";
}

Resolver::Resolver(const Config& config, json game_overrides)
    : config_(config), overrides_(std::move(game_overrides)) {
  if (!overrides_.is_object()) overrides_ = json::object();
}

bool Resolver::IsOverridable(std::string_view key) {
  const Entry* entry = Schema::Instance().Find(key);
  return entry != nullptr && entry->scope != Scope::Global;
}

Resolved Resolver::Resolve(std::string_view key) const {
  const std::string flat_key(key);
  if (IsOverridable(key) && overrides_.contains(flat_key)) {
    return {overrides_.at(flat_key), Layer::Game};
  }

  const Entry* entry = Schema::Instance().Find(key);
  const json value = config_.Get(key);
  if (entry != nullptr && value == entry->default_value) return {value, Layer::Default};
  return {value, Layer::ConfigFile};
}

bool Resolver::GetBool(std::string_view key) const {
  const json value = Value(key);
  return value.is_boolean() && value.get<bool>();
}

std::int64_t Resolver::GetInt(std::string_view key) const {
  const json value = Value(key);
  return value.is_number() ? value.get<std::int64_t>() : 0;
}

double Resolver::GetDouble(std::string_view key) const {
  const json value = Value(key);
  return value.is_number() ? value.get<double>() : 0.0;
}

std::string Resolver::GetString(std::string_view key) const {
  const json value = Value(key);
  return value.is_string() ? value.get<std::string>() : std::string();
}

std::vector<std::string> Resolver::GetStringArray(std::string_view key) const {
  std::vector<std::string> out;
  const json value = Value(key);
  if (!value.is_array()) return out;
  for (const json& item : value) {
    if (item.is_string()) out.push_back(item.get<std::string>());
  }
  return out;
}

json Resolver::EffectiveDocument() const {
  json out = json::object();
  for (const Entry& entry : Schema::Instance().Entries()) {
    const Resolved resolved = Resolve(entry.key);
    out[entry.key] = {
        {"value", resolved.value},
        {"layer", ToString(resolved.layer)},
        {"overridable", IsOverridable(entry.key)},
    };
  }
  return out;
}

}  // namespace mira::config
