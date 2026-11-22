#include "saves/Match.h"

#include <algorithm>

#include "config/Resolver.h"
#include "library/GameIdentity.h"
#include "library/Relocate.h"
#include "saves/Manifest.h"

namespace mira::saves {
namespace {

// Follows aliases to the entry holding the save locations; nullopt when one leads nowhere.
std::optional<store::MetadataStore::SaveEntry> Resolve(const store::MetadataStore& cache,
                                                       const std::string& name) {
  auto entry = cache.FindSaveEntry(name);
  for (int hops = 0; entry && !entry->alias_of.empty() && hops < 5; ++hops)
    entry = cache.FindSaveEntry(entry->alias_of);
  if (!entry || !entry->alias_of.empty()) return std::nullopt;
  return entry;
}

std::string LutrisSlug(const model::Game& game) {
  if (game.source == "lutris") return game.source_ref;
  if (game.source_ref.starts_with(library::kClaimedLutrisPrefix))
    return game.source_ref.substr(library::kClaimedLutrisPrefix.size());
  return {};
}

}  // namespace

std::optional<Match> FindEntry(const config::Config& config, const store::MetadataStore& cache,
                               const model::Game& game) {
  const config::Resolver settings(config, game.overrides);
  const auto found = [&](const std::string& name, const char* rule) -> std::optional<Match> {
    if (name.empty()) return std::nullopt;
    auto entry = Resolve(cache, name);
    if (!entry) return std::nullopt;
    return Match{std::move(*entry), rule};
  };

  // The user's pick; one the manifest no longer has falls through to the rules below.
  if (auto match = found(settings.GetString("saves.manifest_name"), "setting")) return match;

  const library::GameIdentity identity = library::Identify(config, game);
  if (auto match = found(cache.SaveEntryWithId("steam", identity.steam_appid), "steam"))
    return match;
  if (identity.store == "gog" && !identity.store_id.empty()) {
    if (auto match = found(cache.SaveEntryWithId("gog", identity.store_id), "gog")) return match;
  }

  if (const std::string slug = LutrisSlug(game); !slug.empty()) {
    if (auto match = found(cache.SaveEntryWithId("lutris", slug), "lutris")) return match;
    if (auto match = found(cache.SaveEntryWithNorm(Normalize(slug)), "lutris")) return match;
  }

  if (!identity.title.empty()) {
    if (auto match = found(cache.SaveEntryWithNorm(Normalize(identity.title)), "name"))
      return match;
  }
  return found(cache.SaveEntryWithNorm(Normalize(game.name)), "name");
}

}  // namespace mira::saves
