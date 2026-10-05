#include "library/PrefixNaming.h"

#include <format>
#include <system_error>

#include "core/Strings.h"

namespace mira::library {

std::filesystem::path NamedDir(const config::Config& config, const model::Game& game,
                               const std::filesystem::path& root,
                               const std::filesystem::path& current,
                               const std::set<std::filesystem::path>& taken) {
  namespace fs = std::filesystem;
  if (config.GetString("prefix_naming") != "name") return root / game.id;

  const std::string base = strings::Slugify(game.name);  // never empty -- Slugify's own "game" fallback

  std::error_code ec;
  fs::path candidate = root / base;
  for (int suffix = 2; candidate != current && (taken.contains(candidate) || fs::exists(candidate, ec)); ++suffix) {
    candidate = root / std::format("{}-{}", base, suffix);
  }
  return candidate;
}

bool NeedsProvisioning(const std::optional<model::Game>& existing) {
  return !existing || existing->runner_ref.empty() || existing->data_dir.empty() ||
         existing->status == model::GameStatus::Broken || existing->status == model::GameStatus::SettingUp;
}

std::filesystem::path PrefixDir(const config::Config& config, const model::Game& game) {
  return NamedDir(config, game, config.GetPath("prefix_root"));
}

std::filesystem::path PrefixDir(const config::Config& config, const store::GameStore& games, const model::Game& game) {
  std::set<std::filesystem::path> taken;
  for (const model::Game& other : games.All()) {
    if (other.id != game.id && !other.data_dir.empty()) taken.insert(other.data_dir);
  }
  return NamedDir(config, game, config.GetPath("prefix_root"), {}, taken);
}

}  // namespace mira::library
