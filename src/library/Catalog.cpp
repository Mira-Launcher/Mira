#include "library/Catalog.h"

#include "core/Log.h"
#include "library/ILibrarySource.h"
#include "library/SourceRegistry.h"

namespace mira::library {

void MarkTracked(const store::GameStore& games, CatalogEntry& entry) {
  const std::string id = entry.source + "-" + entry.ref;
  if (const auto tracked = games.Find(id)) {
    entry.installed = true;
    entry.game_id = id;
  }
}

std::optional<bool> CatalogCache::Check(const config::Config& config, store::GameStore& games, ILibrarySource& source) {
  Result<std::vector<CatalogEntry>> listed = source.Catalog(config, games);
  if (!listed) {
    // Unconfigured or signed out is the normal case for most sources, and leaves the others listed.
    log::Warn("{} catalog unavailable: {}", source.Name(), listed.error().message);
    return std::nullopt;
  }
  std::vector<store::MetadataStore::CatalogRow> rows;
  rows.reserve(listed->size());
  for (const CatalogEntry& entry : *listed) rows.push_back({entry.ref, entry.title, entry.play_seconds, entry.owned});
  const Result<bool> changed = games.Metadata().WriteCatalog(source.Name(), rows);
  if (!changed) {
    log::Warn("could not store the {} catalog: {}", source.Name(), changed.error().message);
    return true;
  }
  return *changed;
}

Result<std::vector<CatalogEntry>> CatalogCache::List(const config::Config& config, store::GameStore& games,
                                                     api::EventBus& events, Lane& lane, const std::string& source,
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
    } else {
      std::lock_guard lock(mutex_);
      if (checking_.insert(name).second) {
        events.Publish("library.catalog_checking", {{"source", name}});
        lane.Post([this, &config, &games, &events, src = src.get(), name] {
          const auto changed = Check(config, games, *src);
          {
            std::lock_guard lock(mutex_);
            checking_.erase(name);
          }
          events.Publish("library.catalog_checked", {{"source", name}, {"changed", changed.value_or(false)}});
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
      MarkTracked(games, entry);
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

}  // namespace mira::library
