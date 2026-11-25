#include "saves/remote/Apply.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "core/AtomicFile.h"
#include "core/Files.h"
#include "core/Strings.h"
#include "saves/Backups.h"
#include "saves/Resolve.h"

namespace mira::saves::remote {
namespace {
namespace fs = std::filesystem;

constexpr const char* kJournal = "applying.json";

std::string Name(const fs::path& path, bool fold) {
  const std::string name = path.lexically_normal().generic_string();
  return fold ? strings::ToLower(name) : name;
}

// Whether anything in `path` below `folder`, or `path` itself, is a symlink.
bool ThroughSymlink(const fs::path& folder, const fs::path& path) {
  fs::path at = folder;
  std::error_code ec;
  for (const fs::path& part : path.lexically_relative(folder)) {
    if (part == ".") continue;
    at /= part;
    if (fs::is_symlink(fs::symlink_status(at, ec))) return true;
  }
  return fs::is_symlink(fs::symlink_status(path, ec));
}

Error Unsafe(const std::string& path) {
  return {.code = "remote_unsafe",
          .message = std::format("The remote's {} isn't in one of this game's save locations, so "
                                 "nothing was changed.",
                                 path),
          .hint = "If the game keeps saves there, add the folder to its save locations."};
}

Result<void> Replace(const fs::path& from, const fs::path& to, std::int64_t mtime_ns) {
  const fs::path temp = to.parent_path() / ("." + to.filename().string() + ".mira-sync");
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  if (!ec) fs::copy_file(from, temp, fs::copy_options::overwrite_existing, ec);
  if (!ec) fs::last_write_time(temp, FromUnixNs(mtime_ns), ec);
  if (!ec) fs::rename(temp, to, ec);
  if (!ec) return {};
  std::error_code ignored;
  fs::remove(temp, ignored);
  return Err("saves_failed", std::format("Couldn't write {}: {}", to.string(), ec.message()));
}

Result<void> Place(const config::Config& config, const model::Game& game, const Survey& survey,
                   const Revision& revision) {
  const fs::path staging = StagingFolder(config, game);
  const Roots roots = RootsFor(config, game);
  const auto targets = Targets(roots, survey, revision.files);
  if (!targets) return std::unexpected(targets.error());
  std::set<std::string> kept;
  for (std::size_t i = 0; i < targets->size(); ++i) {
    const RevisionFile& file = revision.files[i];
    kept.insert(Name((*targets)[i], roots.windows));
    if (auto placed =
            Replace(staging / "files" / file.root / file.path, (*targets)[i], file.mtime_ns);
        !placed)
      return placed;
  }
  for (const SaveFile& file : LocalFiles(survey)) {
    if (kept.contains(Name(file.absolute, roots.windows))) continue;
    std::error_code ec;
    fs::remove(file.absolute, ec);
    if (ec)
      return Err("saves_failed",
                 std::format("Couldn't remove {}: {}", file.absolute.string(), ec.message()));
  }
  std::error_code ec;
  fs::remove(staging / kJournal, ec);
  fs::remove_all(staging, ec);
  return {};
}

}  // namespace

std::vector<SaveFile> LocalFiles(const Survey& survey) {
  std::map<std::pair<std::string, std::string>, SaveFile> files;
  for (const Location& location : survey.locations)
    for (const SaveFile& file : location.files) files.try_emplace({file.root, file.path}, file);
  std::vector<SaveFile> out;
  for (auto& [key, file] : files) out.push_back(std::move(file));
  return out;
}

std::string LocalMarker(const std::vector<SaveFile>& files) {
  if (files.empty()) return "none";
  std::string text;
  for (const SaveFile& file : files)
    text += std::format("{}\t{}\t{}\t{}\n", file.root, file.path, file.bytes, file.mtime_ns);
  return strings::StableHash(text);
}

Result<std::vector<fs::path>> Targets(const Roots& roots, const Survey& survey,
                                      const std::vector<RevisionFile>& files) {
  std::vector<fs::path> targets;
  for (const RevisionFile& file : files) {
    const fs::path root = RootPath(roots, file.root);
    if (root.empty())
      return Err("saves_unplaceable",
                 "The remote's saves belong in a folder this game doesn't have here.",
                 "A Windows game's saves can't go to its Linux version, or the other way.");
    fs::path target = (root / file.path).lexically_normal();
    if (roots.windows) target = Folded(target);
    fs::path covered;
    for (const Location& location : survey.locations) {
      covered = CoveredBy(location.pattern, roots, target);
      if (!covered.empty()) break;
    }
    if (covered.empty() || ThroughSymlink(covered, target))
      return std::unexpected(Unsafe(file.path));
    std::error_code ec;
    if (fs::is_directory(target, ec)) return std::unexpected(Unsafe(file.path));
    targets.push_back(std::move(target));
  }
  return targets;
}

fs::path StagingFolder(const config::Config& config, const model::Game& game) {
  return GameBackups(config, game.id) / ".remote";
}

Result<void> Apply(const config::Config& config, const model::Game& game, const Survey& survey,
                   const Revision& revision, const std::function<Result<void>()>& before) {
  if (auto targets = Targets(RootsFor(config, game), survey, revision.files); !targets)
    return std::unexpected(targets.error());
  if (auto ready = before(); !ready) return ready;
  if (auto written = WriteFileAtomic(StagingFolder(config, game) / kJournal,
                                     ManifestJson(revision).dump(), "saves_failed", true);
      !written)
    return written;
  return Place(config, game, survey, revision);
}

bool ApplyUnfinished(const config::Config& config, const model::Game& game) {
  std::error_code ec;
  return fs::exists(StagingFolder(config, game) / kJournal, ec);
}

void DropApply(const config::Config& config, const model::Game& game) {
  std::error_code ec;
  fs::remove(StagingFolder(config, game) / kJournal, ec);
  fs::remove_all(StagingFolder(config, game), ec);
}

Result<std::string> FinishApply(const config::Config& config, const model::Game& game,
                                const Survey& survey, const std::function<Result<void>()>& before) {
  const auto text = files::ReadFile(StagingFolder(config, game) / kJournal);
  if (!text) return std::string();
  const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, false);
  const std::string id = doc.is_object() ? doc.value("id", std::string()) : std::string();
  const auto revision = ParseManifest(*text, id);
  if (!revision) return Err("saves_failed", "a sync that was cut short can't be read");
  if (auto ready = before(); !ready) return std::unexpected(ready.error());
  if (auto placed = Place(config, game, survey, *revision); !placed)
    return std::unexpected(placed.error());
  return id;
}

}  // namespace mira::saves::remote
