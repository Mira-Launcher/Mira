#include "core/Json.h"
#include "amazon/AmazonSource.h"

#include <json.hpp>

#include "amazon/AmazonImporter.h"
#include "amazon/Nile.h"
#include "core/StoreErrors.h"
#include "library/StoreProgress.h"

namespace mira::amazon {
namespace {
using nlohmann::json;

Result<void> Download(config::Config& config, store::GameStore& games, api::EventBus& events,
                      const std::string& verb, const std::string& ref) {
  if (auto ready = CheckReady(config); !ready) return ready;
  library::StoreProgress progress(events, "amazon", ref);
  if (auto output = RunNile(config, {verb, ref, "--base-path", config.GetPath("amazon.install_root").string()},
                            [&progress](std::string_view chunk) { progress.Feed(chunk); });
      !output) {
    return std::unexpected(output.error());
  }
  AmazonImporter importer(config, games, events);
  if (auto imported = importer.Import(); !imported) return std::unexpected(imported.error());
  return {};
}

}  // namespace

Result<std::vector<library::CatalogEntry>> AmazonSource::Catalog(const config::Config& config,
                                                                 const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!config.GetBool("amazon.enabled")) return entries;
  if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
  if (auto synced = RunNile(config, {"library", "sync"}); !synced) return std::unexpected(synced.error());

  const json library = ReadNileFile("library.json");
  if (!library.is_array()) return entries;
  for (const json& item : library) {
    const json product = item.is_object() && item.contains("product") ? item["product"] : json::object();
    library::CatalogEntry entry;
    entry.source = "amazon";
    entry.ref = core::JsonString(product, "id");
    if (entry.ref.empty()) continue;
    entry.title = product.contains("title") && product["title"].is_string() ? product["title"].get<std::string>()
                                                                             : entry.ref;
    library::MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

Result<void> AmazonSource::Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                                  const std::string& ref) {
  return Download(config, games, events, "install", ref);
}

Result<void> AmazonSource::Update(config::Config& config, store::GameStore& games, api::EventBus& events,
                                 const std::string& ref) {
  return Download(config, games, events, "update", ref);
}

}  // namespace mira::amazon
