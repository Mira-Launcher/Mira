#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Lane.h"
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
// meaningless data_dir/runner_ref/play_seconds each), so they stay out of
// mira.db: cache.db keeps each store's last answer (CatalogCache), and a
// title becomes a real model::Game only once it's installed.
namespace mira::library {

class ILibrarySource;

struct CatalogEntry {
  std::string source;  // "epic" | "steam"
  std::string ref;     // the source's own stable id: Legendary's app_name, Steam's appid
  std::string title;
  bool installed = false;   // already tracked by Mira (GET /v1/games has it)
  std::string game_id;      // the tracked game's id, if installed
  std::int64_t play_seconds = 0;  // as the source reports it; 0 if it doesn't
  bool owned = true;  // false: listed (e.g. from an itch collection) but not installable
};

// GET /v1/library's listing, from each source's list stored in cache.db with `installed` marked
// now. A source with no stored list is asked now and its answer stored; every other one is
// re-checked on `lane` (once at a time per source) between library.catalog_checking and
// library.catalog_checked {source, changed}. `fresh` asks every source now instead.
class CatalogCache {
public:
  Result<std::vector<CatalogEntry>> List(const config::Config& config, store::GameStore& games,
                                         api::EventBus& events, Lane& lane, const std::string& source,
                                         bool fresh);

private:
  // Asks `source` and stores its answer; null when it couldn't be asked.
  std::optional<bool> Check(const config::Config& config, store::GameStore& games, ILibrarySource& source);

  std::mutex mutex_;
  std::set<std::string> checking_;  // guarded by mutex_
};

// Marks `entry` as already-tracked if Mira has a game for it. Every
// ILibrarySource::Catalog implementation derives a tracked game's id the
// same way ("<source>-<ref>"), so this is a direct lookup rather than a
// scan -- shared here so each source's Catalog() doesn't reimplement it.
void MarkTracked(const store::GameStore& games, CatalogEntry& entry);

}  // namespace mira::library
