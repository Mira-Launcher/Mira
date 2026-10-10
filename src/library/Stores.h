#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "runner/Downloader.h"
#include "runner/StoreTool.h"
#include "store/GameStore.h"

namespace mira::library {

// One storefront that Mira drives through its command-line tool: how to set the tool up, sign in and out, and
// import what it has installed. Routes, the CLI and source removal act on this table, so a store is added in
// one place. (Catalog, install and update are ILibrarySource's.)
struct Store {
  const char* id;            // "gog": the id in settings, game sources, events and routes
  const char* name;          // "GOG"
  const char* tool;          // "gogdl"
  const char* release_kind;  // what runner::ListReleases is asked for to download the tool

  Result<void> (*install_tool)(const config::Config& config, const runner::ReleaseAsset& asset);
  runner::AuthStatus (*status)(const config::Config& config);
  // The page to sign in at; for Amazon it also starts the sign-in the credential completes.
  Result<std::string> (*begin_login)(const config::Config& config);
  // Completes sign-in with what the user pasted: a code, an API key, a session key or a redirect URL.
  Result<void> (*login)(const config::Config& config, const std::string& credential);
  // The credential in text the user copied, or nullopt when it holds none. Stricter than login, which
  // also takes a bare code, so a client can sign in from the clipboard without guessing.
  std::optional<std::string> (*find_credential)(std::string_view text);
  Result<void> (*logout)(const config::Config& config);  // null when the tool keeps the session itself
  // Null when the store has nothing installed to import.
  Result<ImportSummary> (*import)(config::Config& config, store::GameStore& games, api::EventBus& events);
};

const std::vector<Store>& AllStores();
const Store* FindStore(std::string_view id);

}  // namespace mira::library
