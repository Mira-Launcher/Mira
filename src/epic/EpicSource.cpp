#include "core/Json.h"
#include "epic/EpicSource.h"

#include <json.hpp>

#include "epic/EpicImporter.h"
#include "epic/EpicInstaller.h"
#include "core/StoreErrors.h"
#include "epic/Legendary.h"

namespace mira::epic {
using nlohmann::json;

Result<std::vector<library::CatalogEntry>> EpicSource::Catalog(const config::Config& config,
                                                              const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!config.GetBool("epic.enabled")) return entries;

  const Result<json> listed = RunLegendaryJson(config, {"list"});
  if (!listed) return std::unexpected(listed.error());
  if (!listed->is_array()) return entries;

  for (const json& item : *listed) {
    library::CatalogEntry entry;
    entry.source = "epic";
    entry.ref = core::JsonString(item, "app_name");
    if (entry.ref.empty()) continue;
    // `list --json` calls this "app_title"; `list-installed --json` calls
    // the same thing "title" -- accept either.
    entry.title = core::JsonString(item, "app_title", core::JsonString(item, "title"));
    if (entry.title.empty()) entry.title = entry.ref;
    library::MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

Result<void> EpicSource::Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                                const std::string& ref) {
  if (auto ready = CheckReady(config); !ready) return ready;
  EpicInstaller installer(config, games, events);
  return installer.Install(ref);
}

Result<void> EpicSource::Update(config::Config& config, store::GameStore& games, api::EventBus& events,
                               const std::string& ref) {
  if (auto ready = CheckReady(config); !ready) return ready;
  EpicInstaller installer(config, games, events);
  return installer.Update(ref);
}

}  // namespace mira::epic
