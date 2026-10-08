#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "store/GameStore.h"

// What you *own* on a storefront, as opposed to what Mira actually tracks.
//
// These are two different things and conflating them was a mistake worth
// spelling out: a tracked game (model::Game, in mira.db) is something Mira
// manages -- installed, provisioned, launchable, with session history. A
// catalog entry is an entitlement: a title the account owns, most of which
// aren't on disk at all. Persisting entitlements as tracked games bloated a
// file the README promises stays hand-editable (120 Epic rows carrying a
// meaningless data_dir/runner_ref/play_seconds each), so they aren't
// persisted at all now -- they're read through from whatever each source
// already caches, and a title becomes a real model::Game only once it's
// installed.
namespace mira::library {

struct CatalogEntry {
  std::string source;  // "epic" | "steam"
  std::string ref;     // the source's own stable id: Legendary's app_name, Steam's appid
  std::string title;
  bool installed = false;   // already tracked by Mira (GET /v1/games has it)
  std::string game_id;      // the tracked game's id, if installed
  std::int64_t play_seconds = 0;  // as the source reports it; 0 if it doesn't
  bool owned = true;  // false: listed (e.g. from an itch collection) but not installable
};

// Every entry `source` can report, or every source's at once when `source`
// is empty. A source that isn't configured/authenticated contributes
// nothing rather than failing the whole listing -- one broken storefront
// shouldn't hide the others.
Result<std::vector<CatalogEntry>> ListCatalog(const config::Config& config, const store::GameStore& games,
                                             const std::string& source);

// Marks `entry` as already-tracked if Mira has a game for it. Every
// ILibrarySource::Catalog implementation derives a tracked game's id the
// same way ("<source>-<ref>"), so this is a direct lookup rather than a
// scan -- shared here so each source's Catalog() doesn't reimplement it.
void MarkTracked(const store::GameStore& games, CatalogEntry& entry);

}  // namespace mira::library
