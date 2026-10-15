#pragma once

#include <json.hpp>
#include <toml.hpp>

namespace mira::tomljson {

// The daemon's canonical in-memory representation is JSON (it is also the
// wire format), while on-disk settings and game data are TOML for
// human-editability. These are the only two functions that bridge the two;
// everything else works in JSON.
nlohmann::json ToJson(const toml::node& node);
toml::table ToToml(const nlohmann::json& document);      // document must be an object
std::string ToTomlText(const nlohmann::json& document);  // ToToml, serialized

}  // namespace mira::tomljson
