#include "saves/Saves.h"

#include "config/Resolver.h"
#include "saves/Resolve.h"

namespace mira::saves {
namespace {
using nlohmann::json;

json EntryJson(const std::optional<Match>& match) {
  if (!match) return json::object();
  json entry = json::parse(match->entry.entry, nullptr, false);
  return entry.is_object() ? entry : json::object();
}

}  // namespace

Plan PlanFor(const config::Config& config, const model::Game& game,
             const std::optional<Match>& match) {
  const config::Resolver settings(config, game.overrides);
  if (!settings.GetBool("saves.enabled")) return {.status = "off"};
  const json entry = EntryJson(match);
  const bool steam_cloud =
      entry.contains("cloud") && entry["cloud"].is_object() && entry["cloud"].value("steam", false);
  if (game.runner_ref.starts_with("steam:") && settings.GetString("steam.launch_mode") == "steam" &&
      steam_cloud && settings.GetBool("saves.skip_cloud"))
    return {.status = "steam_cloud"};
  return {.backup = true};
}

json Overview(const config::Config& config, const store::MetadataStore& cache,
              const model::Game& game) {
  const std::optional<Match> match = FindEntry(config, cache, game);
  const Plan plan = PlanFor(config, game, match);
  json answer = {{"enabled", plan.status != "off"},
                 {"entry", match ? json(match->entry.name) : json(nullptr)},
                 {"match", match ? json(match->rule) : json(nullptr)},
                 {"locations", json::array()}};
  if (!plan.backup) {
    answer["status"] = plan.status;
    return answer;
  }

  const Roots roots = RootsFor(config, game);
  const json entry = EntryJson(match);
  std::vector<std::string> patterns = FileLocations(entry, roots);
  for (std::string& extra :
       config::Resolver(config, game.overrides).GetStringArray("saves.extra_paths"))
    patterns.push_back(std::move(extra));
  bool found = false;
  for (const std::string& pattern : patterns) {
    const Location location = Find(pattern, roots);
    std::uintmax_t bytes = 0;
    for (const SaveFile& file : location.files) bytes += file.bytes;
    json item = {{"pattern", location.pattern},
                 {"path", location.path},
                 {"files", location.files.size()},
                 {"bytes", bytes},
                 {"skipped", location.skipped}};
    if (!location.problem.empty()) item["problem"] = location.problem;
    answer["locations"].push_back(std::move(item));
    found = found || !location.files.empty();
  }
  const json registry = entry.value("registry", json::object());
  if (patterns.empty())
    answer["status"] = registry.is_object() && !registry.empty() ? "registry_only" : "no_entry";
  else
    answer["status"] = found ? "ready" : "no_files";
  return answer;
}

}  // namespace mira::saves
