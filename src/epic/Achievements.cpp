#include "epic/Achievements.h"

#include <fstream>
#include <json.hpp>
#include <map>

#include "core/Json.h"
#include "epic/Legendary.h"

namespace mira::epic {
namespace {
using achievements::Achievement;
using achievements::GameAchievements;
using nlohmann::json;

// legendary's groups of its answer, each in an order of its own.
constexpr const char* kGroups[] = {"completed", "in_progress", "uninitiated", "hidden"};

double Rarity(const json& entry) {
  const json rarity = entry.is_object() ? entry.value("rarity", json()) : json();
  const auto percent = rarity.is_object() ? rarity.find("percent") : rarity.end();
  return rarity.is_object() && percent != rarity.end() && percent->is_number()
             ? percent->get<double>()
             : -1;
}

// Epic's own definitions, in its order, from what `legendary achievements` just cached.
std::vector<json> CachedDefinitions(const std::string& app_name) {
  std::ifstream file(LegendaryMetadataFile(app_name));
  const json meta = json::parse(file, nullptr, false);
  const json list = meta.is_object() ? meta.value("achievements", json::object()) : json();
  const json items = list.is_object() ? list.value("achievements", json()) : json();
  std::vector<json> definitions;
  if (!items.is_array()) return definitions;
  for (const json& item : items) {
    const json definition = item.is_object() ? item.value("achievement", json()) : json();
    if (definition.is_object() && !core::JsonString(definition, "name").empty())
      definitions.push_back(definition);
  }
  return definitions;
}

}  // namespace

Result<GameAchievements> FetchAchievements(const config::Config& config,
                                           const std::string& app_name) {
  if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
  auto output = RunLegendary(config, {"achievements", app_name, "--json"});
  if (!output) {
    // argparse's answer to a subcommand it doesn't know.
    if (output.error().message.find("invalid choice") != std::string::npos) {
      return Err("legendary_too_old", "this version of Legendary can't read achievements",
                 "Update Legendary to 0.21.1 or newer.", Fix::Source("epic", "setup"));
    }
    return std::unexpected(output.error());
  }
  GameAchievements set{.source = "epic", .fetched_at = 0, .items = {}};
  const json answer = core::ParseJsonTail(*output);
  // A game without achievements prints only a log line.
  if (!answer.is_object()) {
    if (output->find("No achievements found") != std::string::npos) return set;
    return Err("legendary_json_error", "legendary's output wasn't valid JSON");
  }

  // The account's state of each, by name.
  std::map<std::string, Achievement> state;
  for (const char* group : kGroups) {
    const json entries = answer.value(group, json());
    if (!entries.is_array()) continue;
    for (const json& entry : entries) {
      Achievement item;
      item.key = core::JsonString(entry, "name");
      if (item.key.empty()) continue;
      item.name = core::JsonString(entry, "display_name");
      item.description = core::JsonString(entry, "description");
      item.icon_unlocked = core::JsonString(entry, "icon_link");
      item.icon_locked = item.icon_unlocked;
      item.hidden = std::string_view(group) == "hidden";
      item.unlocked = core::JsonBool(entry, "unlocked");
      item.unlocked_at = achievements::ParseTime(core::JsonString(entry, "unlock_date"));
      item.rarity = Rarity(entry);
      item.xp = core::JsonInt(entry, "xp");
      state.emplace(item.key, std::move(item));
    }
  }

  // Epic's order and both pictures when legendary's cache has them; legendary's groups otherwise.
  const std::vector<json> definitions = CachedDefinitions(app_name);
  for (const json& definition : definitions) {
    auto found = state.find(core::JsonString(definition, "name"));
    if (found == state.end()) continue;
    Achievement item = std::move(found->second);
    state.erase(found);
    item.hidden = core::JsonBool(definition, "hidden") && !item.unlocked;
    if (std::string icon = core::JsonString(definition, "unlockedIconLink"); !icon.empty())
      item.icon_unlocked = std::move(icon);
    if (std::string icon = core::JsonString(definition, "lockedIconLink"); !icon.empty())
      item.icon_locked = std::move(icon);
    // A hidden one's locked text is a placeholder; the client masks it until unlocked anyway.
    if (item.hidden) {
      item.name = core::JsonString(definition, "unlockedDisplayName", item.name);
      item.description = core::JsonString(definition, "unlockedDescription", item.description);
    }
    set.items.push_back(std::move(item));
  }
  for (const char* group : kGroups) {
    const json entries = answer.value(group, json());
    if (!entries.is_array()) continue;
    for (const json& entry : entries) {
      auto found = state.find(core::JsonString(entry, "name"));
      if (found == state.end()) continue;
      set.items.push_back(std::move(found->second));
      state.erase(found);
    }
  }
  return set;
}

}  // namespace mira::epic
