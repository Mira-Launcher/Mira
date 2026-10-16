#include "library/PrefixNaming.h"

#include <sys/stat.h>

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

  const std::string base =
      strings::Slugify(game.name);  // never empty -- Slugify's own "game" fallback

  std::error_code ec;
  fs::path candidate = root / base;
  for (int suffix = 2;
       candidate != current && (taken.contains(candidate) || fs::exists(candidate, ec)); ++suffix) {
    candidate = root / std::format("{}-{}", base, suffix);
  }
  return candidate;
}

bool NeedsProvisioning(const std::optional<model::Game>& existing) {
  return !existing || existing->runner_ref.empty() || existing->data_dir.empty() ||
         existing->status == model::GameStatus::Broken ||
         existing->status == model::GameStatus::SettingUp;
}

namespace {

// The disk `path` is on, or of the nearest folder above it that exists.
std::optional<dev_t> DiskOf(std::filesystem::path path) {
  for (; !path.empty(); path = path.parent_path()) {
    struct stat info{};
    if (::stat(path.c_str(), &info) == 0) return info.st_dev;
    if (path == path.parent_path()) break;
  }
  return std::nullopt;
}

}  // namespace

std::filesystem::path PrefixRoot(const config::Config& config, const model::Game& game) {
  const std::filesystem::path prefix_root = config.GetPath("prefix_root");
  if (game.install_path.empty()) return prefix_root;
  const std::optional<dev_t> disk = DiskOf(game.install_path);
  if (!disk || DiskOf(prefix_root) == disk) return prefix_root;
  for (const std::filesystem::path& folder : config.GetPathArray("mira_folders")) {
    if (DiskOf(folder) == disk) return folder / "prefixes";
  }
  return prefix_root;
}

std::vector<std::filesystem::path> PrefixRoots(const config::Config& config) {
  std::vector<std::filesystem::path> roots = {config.GetPath("prefix_root")};
  for (const std::filesystem::path& folder : config.GetPathArray("mira_folders"))
    roots.push_back(folder / "prefixes");
  return roots;
}

std::filesystem::path PrefixDir(const config::Config& config, const model::Game& game) {
  return NamedDir(config, game, PrefixRoot(config, game));
}

std::filesystem::path PrefixDir(const config::Config& config, const store::GameStore& games,
                                const model::Game& game) {
  std::set<std::filesystem::path> taken;
  for (const model::Game& other : games.All()) {
    if (other.id != game.id && !other.data_dir.empty()) taken.insert(other.data_dir);
  }
  return NamedDir(config, game, PrefixRoot(config, game), {}, taken);
}

}  // namespace mira::library
