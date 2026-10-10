#include "migrate/Sources.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "core/Log.h"
#include "launchers/Launchers.h"
#include "library/Stores.h"

namespace mira::migrate {
namespace {
using nlohmann::json;

// The sources that had an on/off key in settings.toml.
constexpr std::string_view kToggled[] = {"steam",   "epic",   "gog",     "itch",      "humble", "amazon",
                                         "lutris", "battlenet", "ubisoft", "ea", "office"};

}  // namespace

void ImportSourceState(const config::Config& config, store::GameStore& games) {
  int set_up = 0;
  int off = 0;
  int hidden = 0;
  const auto mark = [&](const std::string& id) {
    games.MarkSourceAdded(id, false);
    ++set_up;
  };

  for (const library::Store& source : library::AllStores()) {
    if (source.status && source.status(config).authenticated) mark(source.id);
  }
  for (const launchers::Launcher& launcher : launchers::All()) {
    if (launchers::Installed(games, launcher)) mark(launcher.id);
  }
  if (!config.GetString("steam.web_api_key").empty()) mark("steam");

  const json document = config.Document();
  for (const std::string_view id : kToggled) {
    const std::string key(id);
    if (!document.contains(key) || !document[key].is_object()) continue;
    const json& section = document[key];
    if (!section.contains("enabled") || section["enabled"] != false) continue;
    store::SourceState state = games.Source(key);
    state.enabled = false;
    games.SaveSource(state);
    ++off;
  }

  const json frontend = config.FrontendSettings();
  if (frontend.contains("hidden_sources") && frontend["hidden_sources"].is_array()) {
    for (const json& id : frontend["hidden_sources"]) {
      if (!id.is_string()) continue;
      store::SourceState state = games.Source(id.get<std::string>());
      state.in_sidebar = false;
      games.SaveSource(state);
      ++hidden;
    }
  }
  if (frontend.contains("source_order") && frontend["source_order"].is_array()) {
    std::vector<std::string> order;
    for (const json& id : frontend["source_order"]) {
      if (id.is_string()) order.push_back(id.get<std::string>());
    }
    // As the GUI read it: a source the order doesn't name goes right after its neighbour in the
    // default order, so Local, first there, leads.
    const std::vector<std::string> all = {"local",  "steam",     "epic",    "gog", "itch",   "amazon",
                                          "humble", "battlenet", "ubisoft", "ea",  "office", "lutris"};
    for (auto source = all.begin(); source != all.end(); ++source) {
      if (std::ranges::find(order, *source) != order.end()) continue;
      auto at = order.begin();
      for (auto before = source; before != all.begin();) {
        if (const auto it = std::ranges::find(order, *--before); it != order.end()) {
          at = it + 1;
          break;
        }
      }
      order.insert(at, *source);
    }
    games.SetSourceOrder(order);
  }

  log::Info("sources from settings: {} set up, {} off, {} hidden", set_up, off, hidden);
}

}  // namespace mira::migrate
