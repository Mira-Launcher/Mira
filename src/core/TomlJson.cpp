#include "core/TomlJson.h"

#include <sstream>

namespace mira::tomljson {
namespace {
using nlohmann::json;

json ArrayToJson(const toml::array& array) {
  json out = json::array();
  array.for_each([&](auto&& element) { out.push_back(ToJson(element)); });
  return out;
}

json TableToJson(const toml::table& table) {
  json out = json::object();
  table.for_each(
      [&](const toml::key& key, auto&& value) { out[std::string(key.str())] = ToJson(value); });
  return out;
}

}  // namespace

json ToJson(const toml::node& node) {
  // node.value<T>() performs permissive conversion (a bool also satisfies
  // value<int64_t>()), which would silently turn TOML booleans into JSON
  // numbers. The exact type predicates below preserve the TOML type.
  if (node.is_table()) return TableToJson(*node.as_table());
  if (node.is_array()) return ArrayToJson(*node.as_array());
  if (node.is_boolean()) return node.value_exact<bool>().value_or(false);
  if (node.is_integer()) return node.value_exact<std::int64_t>().value_or(0);
  if (node.is_floating_point()) return node.value_exact<double>().value_or(0.0);
  if (node.is_string()) return node.value_exact<std::string>().value_or(std::string());
  return json();
}

namespace {

toml::array JsonArrayToToml(const json& array) {
  toml::array out;
  for (const json& item : array) {
    if (item.is_object()) {
      out.push_back(ToToml(item));
    } else if (item.is_array()) {
      out.push_back(JsonArrayToToml(item));
    } else if (item.is_string()) {
      out.push_back(item.get<std::string>());
    } else if (item.is_boolean()) {
      out.push_back(item.get<bool>());
    } else if (item.is_number_integer()) {
      out.push_back(item.get<std::int64_t>());
    } else if (item.is_number()) {
      out.push_back(item.get<double>());
    }
    // null entries are dropped: TOML has no null.
  }
  return out;
}

}  // namespace

toml::table ToToml(const json& document) {
  toml::table out;
  if (!document.is_object()) return out;

  for (const auto& [key, value] : document.items()) {
    if (value.is_object()) {
      out.insert(key, ToToml(value));
    } else if (value.is_array()) {
      out.insert(key, JsonArrayToToml(value));
    } else if (value.is_string()) {
      out.insert(key, value.get<std::string>());
    } else if (value.is_boolean()) {
      out.insert(key, value.get<bool>());
    } else if (value.is_number_integer()) {
      out.insert(key, value.get<std::int64_t>());
    } else if (value.is_number()) {
      out.insert(key, value.get<double>());
    }
    // null values are omitted rather than written as an empty key.
  }
  return out;
}

std::string ToTomlText(const json& document) {
  std::ostringstream text;
  text << ToToml(document);
  return text.str();
}

}  // namespace mira::tomljson
