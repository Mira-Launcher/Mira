#include "launchers/OfficeSource.h"

#include <algorithm>
#include <format>

#include "launchers/Launchers.h"
#include "launchers/Office.h"

namespace mira::launchers {

Result<std::vector<library::CatalogEntry>> OfficeSource::Catalog(const config::Config& /*config*/,
                                                                 const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!Installed(games, *Find("office"))) return entries;
  for (const office::App& app : office::Apps()) {
    library::CatalogEntry entry;
    entry.source = "office";
    entry.ref = app.ref;
    entry.title = app.name;
    library::MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

Result<void> OfficeSource::Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                                   const std::string& ref) {
  const auto apps = office::Apps();
  if (std::ranges::find(apps, ref, &office::App::ref) == apps.end()) {
    return Err("unknown_app", std::format("Microsoft 365 has no app \"{}\"", ref));
  }
  return AddOfficeApps(config, games, events, {ref});
}

}  // namespace mira::launchers
