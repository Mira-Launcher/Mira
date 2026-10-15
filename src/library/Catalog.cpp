#include "library/Catalog.h"

#include "core/Log.h"
#include "library/ILibrarySource.h"
#include "library/SourceRegistry.h"
#include "model/Types.h"

namespace mira::library {
namespace {

// How old a stored list gets before listing every store asks them again; one store's own list is
// asked each time.
constexpr std::int64_t kRecheckSeconds = 12 * 60 * 60;

}  // namespace

std::string SteamStorePage(const std::string& appid) {
  return "https://store.steampowered.com/app/" + appid;
}

std::string StorePage(store::GameStore& games, const model::Game& game) {
  if (game.source == "steam") {
    // A Steam game's appid is in its runner, "steam:<appid>".
    constexpr std::string_view kPrefix = "steam:";
    if (!game.runner_ref.starts_with(kPrefix) || game.runner_ref.size() == kPrefix.size())
      return {};
    return SteamStorePage(game.runner_ref.substr(kPrefix.size()));
  }
  if (game.source.empty() || game.source_ref.empty()) return {};
  return games.Metadata().CatalogUrl(game.source, game.source_ref);
}

void MarkTracked(const store::GameStore& games, CatalogEntry& entry) {
  const std::string id = entry.source + "-" + entry.ref;
  if (games.Contains(id)) {
    entry.installed = true;
    entry.game_id = id;
  }
}

std::optional<bool> CatalogCache::Check(const config::Config& config, store::GameStore& games,
                                        ILibrarySource& source) {
  std::uint64_t generation = 0;
  {
    std::lock_guard lock(mutex_);
    generation = generations_[source.Name()];
  }
  Result<std::vector<CatalogEntry>> listed = source.Catalog(config, games);
  if (!listed) {
    // Unconfigured or signed out is the normal case for most sources, and leaves the others listed.
    log::Warn("{} catalog unavailable: {}", source.Name(), listed.error().message);
    return std::nullopt;
  }
  std::vector<store::MetadataStore::CatalogRow> rows;
  rows.reserve(listed->size());
  for (const CatalogEntry& entry : *listed)
    rows.push_back({entry.ref, entry.title, entry.play_seconds, entry.owned, entry.url});
  // Held through the write so a sign-out can't land between the check and it.
  std::lock_guard lock(mutex_);
  if (generations_[source.Name()] != generation) return std::nullopt;
  const Result<bool> changed = games.Metadata().WriteCatalog(source.Name(), rows);
  if (!changed) {
    log::Warn("could not store the {} catalog: {}", source.Name(), changed.error().message);
    return true;
  }
  return *changed;
}

void CatalogCache::SignedOut(const std::string& source) {
  std::lock_guard lock(mutex_);
  ++generations_[source];
}

Result<std::vector<CatalogEntry>> CatalogCache::List(const config::Config& config,
                                                     store::GameStore& games, api::EventBus& events,
                                                     Lane& lane, const std::string& source,
                                                     bool fresh) {
  if (!source.empty() && FindSource(source) == nullptr) {
    return Err("unknown_source", "no catalog source named \"" + source + "\"");
  }

  std::vector<CatalogEntry> entries;
  for (const auto& src : AllSources()) {
    const std::string name = src->Name();
    if (!source.empty() && name != source) continue;
    auto rows = fresh ? std::nullopt : games.Metadata().ReadCatalog(name);
    if (!rows) {
      if (const auto changed = Check(config, games, *src); changed && *changed) {
        events.Publish("library.catalog_checked", {{"source", name}, {"changed", true}});
      }
      rows = games.Metadata().ReadCatalog(name);
      if (!rows) continue;
    } else if (!source.empty() ||
               model::NowSeconds() - games.Metadata().CatalogCheckedAt(name) >= kRecheckSeconds) {
      std::lock_guard lock(mutex_);
      if (checking_.insert(name).second) {
        events.Publish("library.catalog_checking", {{"source", name}});
        lane.Post([this, &config, &games, &events, src = src.get(), name] {
          const auto changed = Check(config, games, *src);
          {
            std::lock_guard lock(mutex_);
            checking_.erase(name);
          }
          events.Publish("library.catalog_checked",
                         {{"source", name}, {"changed", changed.value_or(false)}});
        });
      }
    }
    for (const store::MetadataStore::CatalogRow& row : *rows) {
      CatalogEntry entry;
      entry.source = name;
      entry.ref = row.ref;
      entry.title = row.title;
      entry.play_seconds = row.play_seconds;
      entry.owned = row.owned;
      entry.url = row.url;
      MarkTracked(games, entry);
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

}  // namespace mira::library
