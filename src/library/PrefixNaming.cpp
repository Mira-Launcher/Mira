#include "library/PrefixNaming.h"

#include <sys/stat.h>

#include <algorithm>
#include <format>
#include <system_error>
#include <utility>

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

// The prefixes folder of the Mira folder on `disk`, when one is there.
std::optional<std::filesystem::path> MiraPrefixesOn(const config::Config& config, dev_t disk) {
  for (const std::filesystem::path& folder : config.GetPathArray("mira_folders")) {
    if (DiskOf(folder) == disk) return folder / "prefixes";
  }
  return std::nullopt;
}

}  // namespace

std::filesystem::path PrefixRoot(const config::Config& config, const model::Game& game) {
  const std::filesystem::path prefix_root = config.GetPath("prefix_root");
  if (game.install_path.empty()) return prefix_root;
  const std::optional<dev_t> disk = DiskOf(game.install_path);
  if (!disk || disk == DiskOf(prefix_root)) return prefix_root;
  return MiraPrefixesOn(config, *disk).value_or(prefix_root);
}

bool OnSameDisk(const std::filesystem::path& a, const std::filesystem::path& b) {
  const std::optional<dev_t> disk = DiskOf(a);
  return disk && disk == DiskOf(b);
}

std::vector<std::filesystem::path> PrefixRoots(const config::Config& config) {
  std::vector<std::filesystem::path> roots = {config.GetPath("prefix_root")};
  for (const std::filesystem::path& folder : config.GetPathArray("mira_folders"))
    roots.push_back(folder / "prefixes");
  return roots;
}

std::optional<std::filesystem::path> PrefixInGameFolder(const config::Config& config,
                                                        const model::Game& game) {
  std::error_code ec;
  if (game.install_path.empty() || !std::filesystem::is_directory(game.install_path, ec))
    return std::nullopt;
  const std::optional<dev_t> disk = DiskOf(game.install_path);
  if (!disk || disk == DiskOf(config.GetPath("prefix_root")) || MiraPrefixesOn(config, *disk))
    return std::nullopt;
  // A loose program right in a library folder has no folder of its own to keep one in.
  const std::filesystem::path folder = std::filesystem::path(game.install_path).lexically_normal();
  for (const std::filesystem::path& root : config.GetPathArray("library_roots")) {
    if (root.lexically_normal() == folder) return std::nullopt;
  }
  return folder / "prefix";
}

std::filesystem::path PrefixDir(const config::Config& config, const model::Game& game) {
  if (auto own = PrefixInGameFolder(config, game)) return *own;
  return NamedDir(config, game, PrefixRoot(config, game));
}

std::filesystem::path PrefixDir(const config::Config& config, const store::GameStore& games,
                                const model::Game& game) {
  if (const std::string kept = games.KeptDataDir(game.id); !kept.empty()) return kept;
  if (auto own = PrefixInGameFolder(config, game)) return *own;
  std::set<std::filesystem::path> taken;
  for (const model::Game& other : games.All()) {
    if (other.id != game.id && !other.data_dir.empty()) taken.insert(other.data_dir);
  }
  return NamedDir(config, game, PrefixRoot(config, game), {}, taken);
}

}  // namespace mira::library
