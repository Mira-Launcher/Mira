#include "saves/Saves.h"

#include "config/Resolver.h"
#include "saves/Backups.h"

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

Survey SurveyFor(const config::Config& config, const store::MetadataStore& cache,
                 const model::Game& game) {
  Survey survey{.match = FindEntry(config, cache, game)};
  survey.plan = PlanFor(config, game, survey.match);
  const Roots roots = RootsFor(config, game);
  const json entry = EntryJson(survey.match);
  std::vector<std::string> patterns = FileLocations(entry, roots);
  for (std::string& extra :
       config::Resolver(config, game.overrides).GetStringArray("saves.extra_paths"))
    patterns.push_back(std::move(extra));
  for (const std::string& pattern : patterns) survey.locations.push_back(Find(pattern, roots));
  const json registry = entry.value("registry", json::object());
  survey.registry = registry.is_object() && !registry.empty();
  return survey;
}

json Overview(const config::Config& config, const store::MetadataStore& cache,
              const model::Game& game) {
  const Survey survey = SurveyFor(config, cache, game);
  json answer = {{"enabled", survey.plan.status != "off"},
                 {"entry", survey.match ? json(survey.match->entry.name) : json(nullptr)},
                 {"match", survey.match ? json(survey.match->rule) : json(nullptr)},
                 {"locations", json::array()},
                 {"snapshots", json::array()}};
  bool found = false;
  const std::vector<std::string> extra =
      config::Resolver(config, game.overrides).GetStringArray("saves.extra_paths");
  for (const Location& location : survey.locations) {
    std::uintmax_t bytes = 0;
    for (const SaveFile& file : location.files) bytes += file.bytes;
    json item = {{"pattern", location.pattern},
                 {"path", location.path},
                 {"files", location.files.size()},
                 {"bytes", bytes},
                 {"skipped", location.skipped},
                 {"extra", std::ranges::contains(extra, location.pattern)}};
    if (!location.problem.empty()) item["problem"] = location.problem;
    answer["locations"].push_back(std::move(item));
    found = found || !location.files.empty();
  }
  const std::filesystem::path dir = GameBackups(config, game.id);
  const std::vector<Snapshot> snapshots = ListSnapshots(dir);
  for (const Snapshot& snapshot : snapshots) answer["snapshots"].push_back(SnapshotJson(snapshot));
  if (json failure = LastFailure(dir); !failure.is_null())
    answer["last_error"] = std::move(failure);
  const Roots roots = RootsFor(config, game);
  answer["folders"] = {{"install", roots.install.string()}, {"prefix", roots.prefix.string()}};

  if (!survey.plan.backup)
    answer["status"] = survey.plan.status;
  else if (!snapshots.empty())
    answer["status"] = "kept";
  else if (survey.locations.empty())
    answer["status"] = survey.registry ? "registry_only" : "no_entry";
  else
    answer["status"] = found ? "ready" : "no_files";
  return answer;
}

}  // namespace mira::saves
