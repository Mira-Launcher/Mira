#include "runner/RefMigration.h"

#include <algorithm>
#include <cctype>
#include <optional>

#include "config/Schema.h"
#include "core/Log.h"
#include "runner/RunnerRegistry.h"

namespace mira::runner {
namespace {

// What one package's releases share: the name before its version
// ("cachyos-" for "cachyos-11.0-20261005-slr", "proton-" for "proton-11.0-2c-x86_64").
std::string Stem(const std::string& release) {
  return release.substr(0, release.find_first_of("0123456789"));
}

// The release's first number, its major version ("11" for "proton-11.0-2c").
std::string Major(const std::string& release) {
  const std::size_t start = release.find_first_of("0123456789");
  if (start == std::string::npos) return {};
  return release.substr(start, release.find_first_not_of("0123456789", start) - start);
}

// Stricter: the whole name with its digits masked, to tell apart two packages sharing a stem.
std::string Masked(std::string release) {
  std::ranges::replace_if(release, [](unsigned char c) { return std::isdigit(c); }, '#');
  return release;
}

// A folder named without a version ("proton-cachyos-slr") holds whatever version its package
// ships, so a new major version is still the same runner. "Proton 9.0" is not "Proton 8.0".
bool RollingFolder(const model::RunnerBuild& build) {
  return std::ranges::none_of(build.name, [](unsigned char c) { return std::isdigit(c); });
}

// The one build `matches` picks out of `builds`, or nothing when none or several do.
template <typename Pred>
const model::RunnerBuild* OnlyOne(const std::vector<const model::RunnerBuild*>& builds, Pred matches) {
  const model::RunnerBuild* found = nullptr;
  for (const model::RunnerBuild* build : builds) {
    if (!matches(*build)) continue;
    if (found) return nullptr;
    found = build;
  }
  return found;
}

}  // namespace

int MigrateInPlaceRefs(config::Config& config, store::GameStore& games) {
  const RunnerRegistry registry(config);
  std::vector<model::RunnerBuild> in_place;
  std::vector<model::RunnerBuild> all = registry.DiscoverAll();
  std::ranges::copy_if(all, std::back_inserter(in_place),
                       [](const model::RunnerBuild& build) { return build.name != build.release; });
  if (in_place.empty()) return 0;

  // The folder-named reference an old release name stands for, if any.
  const auto migrated = [&](const std::string& ref) -> std::optional<std::string> {
    const auto colon = ref.find(':');
    if (colon == std::string::npos) return std::nullopt;
    const std::string kind = ref.substr(0, colon);
    const std::string name = ref.substr(colon + 1);
    const std::string current_kind = kind == "proton_umu" ? "proton" : kind;  // the old spelling
    if (std::ranges::any_of(all, [&](const model::RunnerBuild& b) { return b.kind == current_kind && b.name == name; })) {
      return std::nullopt;  // still there
    }
    std::vector<const model::RunnerBuild*> same_stem;
    for (const model::RunnerBuild& build : in_place) {
      if (build.kind != current_kind) continue;
      if (build.release == name) return build.Reference();
      if (!Stem(name).empty() && Stem(build.release) == Stem(name)) same_stem.push_back(&build);
    }
    // Only the same major version, unless the folder isn't versioned; ambiguity migrates nothing.
    std::vector<const model::RunnerBuild*> same_major;
    std::ranges::copy_if(same_stem, std::back_inserter(same_major),
                         [&](const model::RunnerBuild* b) { return Major(b->release) == Major(name); });
    const model::RunnerBuild* match = nullptr;
    if (same_major.size() == 1) {
      match = same_major.front();
    } else if (same_major.size() > 1) {
      match = OnlyOne(same_major, [&](const model::RunnerBuild& b) { return Masked(b.release) == Masked(name); });
    } else {
      match = OnlyOne(same_stem, RollingFolder);
    }
    if (match) return match->Reference();
    return std::nullopt;
  };

  int changed = 0;
  std::vector<const config::Entry*> runner_keys;
  for (const config::Entry& entry : config::Schema::Instance().Entries()) {
    if (!entry.is_runner_ref) continue;
    runner_keys.push_back(&entry);
    if (const auto to = migrated(config.GetString(entry.key))) {
      log::Info("{}: {} is now {}", entry.key, config.GetString(entry.key), *to);
      if (config.Set(entry.key, *to)) ++changed;
    }
  }

  for (const model::Game& game : games.All()) {
    auto to = migrated(game.runner_ref);
    nlohmann::json overrides = game.overrides;
    bool overrides_changed = false;
    for (const config::Entry* entry : runner_keys) {
      if (!overrides.contains(entry->key) || !overrides[entry->key].is_string()) continue;
      if (auto override_to = migrated(overrides[entry->key].get<std::string>())) {
        overrides[entry->key] = *override_to;
        overrides_changed = true;
      }
    }
    if (!to && !overrides_changed) continue;
    log::Info("{}: runner {} is now {}", game.id, game.runner_ref, to.value_or(game.runner_ref));
    const auto updated = games.Update(game.id, [&](model::Game& g) {
      if (to) g.runner_ref = *to;
      if (overrides_changed) g.overrides = overrides;
    });
    if (updated) ++changed;
  }
  return changed;
}

}  // namespace mira::runner
