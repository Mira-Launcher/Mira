#include "library/Stores.h"

#include "amazon/AmazonImporter.h"
#include "amazon/Nile.h"
#include "epic/EpicImporter.h"
#include "epic/Legendary.h"
#include "gog/Gog.h"
#include "gog/GogImporter.h"
#include "humble/Humble.h"
#include "itch/Itch.h"
#include "itch/ItchImporter.h"

namespace mira::library {
namespace {

template <typename Importer>
Result<ImportSummary> ImportWith(config::Config& config, store::GameStore& games, api::EventBus& events) {
  Importer importer(config, games, events);
  return importer.Import();
}

std::vector<Store> BuildStores() {
  return {
      {"epic", "Epic Games", "legendary", "legendary", epic::DetectLegendary, epic::InstallLegendaryBinary,
       epic::Status, [](const config::Config&) -> Result<std::string> { return std::string(epic::kLoginUrl); },
       epic::Login, epic::Logout, ImportWith<epic::EpicImporter>},
      {"gog", "GOG", "gogdl", "gog", gog::DetectGog, gog::InstallGogBinary, gog::Status,
       [](const config::Config&) -> Result<std::string> { return std::string(gog::kLoginUrl); }, gog::Login,
       gog::Logout, ImportWith<gog::GogImporter>},
      {"amazon", "Amazon Games", "nile", "amazon", amazon::DetectNile, amazon::InstallNileBinary, amazon::Status,
       amazon::BeginLogin, amazon::FinishLogin, amazon::Logout, ImportWith<amazon::AmazonImporter>},
      {"itch", "itch.io", "butler", "itch", itch::DetectButler, itch::InstallButlerBinary, itch::Status,
       [](const config::Config&) -> Result<std::string> { return std::string(itch::kApiKeysUrl); }, itch::Login,
       itch::Logout, ImportWith<itch::ItchImporter>},
      {"humble", "Humble Bundle", "humble-cli", "humble", humble::DetectHumbleCli, humble::InstallHumbleCliBinary,
       humble::Status,
       [](const config::Config&) -> Result<std::string> { return std::string(humble::kLoginUrl); }, humble::Login,
       nullptr, nullptr},
  };
}

}  // namespace

const std::vector<Store>& AllStores() {
  static const std::vector<Store> stores = BuildStores();
  return stores;
}

const Store* FindStore(std::string_view id) {
  for (const Store& store : AllStores()) {
    if (store.id == id) return &store;
  }
  return nullptr;
}

}  // namespace mira::library
