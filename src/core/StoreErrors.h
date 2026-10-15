#pragma once

#include <format>
#include <string>
#include <string_view>

#include "core/Result.h"

namespace mira {

// A store's command-line tool (legendary, gogdl, ...) isn't installed.
// `source` is the store's id ("epic"), `store` its name ("Epic Games").
inline std::unexpected<Error> StoreToolMissing(std::string_view source, std::string_view store,
                                               std::string_view tool) {
  return Err(std::format("{}_missing", tool == "humble-cli" ? "humble_cli" : tool),
             std::format("{} needs {}, which isn't installed", store, tool),
             std::format("Set up {} to download it.", store),
             Fix::Source(std::string(source), "setup"));
}

inline std::unexpected<Error> StoreNotSignedIn(std::string_view source, std::string_view store) {
  return Err("not_authenticated", std::format("you aren't signed in to {}", store),
             std::format("Sign in to {}.", store), Fix::Source(std::string(source), "login"));
}

// A Windows launcher (Battle.net, ...) that hasn't been installed into its prefix.
inline std::unexpected<Error> LauncherNotInstalled(std::string_view id, std::string_view name) {
  return Err("launcher_not_installed", std::format("{} isn't installed", name),
             std::format("Install {}.", name), Fix::Source(std::string(id), "install"));
}

}  // namespace mira
