#include "saves/remote/Revisions.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <map>
#include <random>
#include <set>

#include "core/AtomicFile.h"
#include "core/Files.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::saves::remote {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kFormat = 1;
constexpr const char* kManifest = "manifest.json";
constexpr std::size_t kMaxFiles = 20000;
constexpr std::array<std::string_view, 8> kRoots = {
    "install", "prefix", "prefix_user", "steam_library", "steam", "home", "xdg_data", "xdg_config"};

bool SafeName(std::string_view name) {
  return !name.empty() && name.size() <= 128 && name.front() != '-' &&
         std::ranges::all_of(
             name, [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_'; });
}

bool SafePath(std::string_view path) {
  if (path.empty() || path.size() > 4096) return false;
  if (std::ranges::any_of(path, [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
    return false;
  for (const std::string_view part : strings::Split(path, '/'))
    if (part.empty() || part == "." || part == "..") return false;
  return true;
}

bool SafeSum(std::string_view sum) {
  return sum.size() == 64 && std::ranges::all_of(sum, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// No file twice, and none under another, where case doesn't count.
bool NoClash(const std::vector<RevisionFile>& files) {
  std::set<std::string> names;
  for (const RevisionFile& file : files)
    if (!names.insert(strings::ToLower(file.root + "/" + file.path)).second) return false;
  for (const std::string& name : names)
    for (std::size_t at = name.find('/'); at != std::string::npos; at = name.find('/', at + 1))
      if (names.contains(name.substr(0, at))) return false;
  return true;
}

std::string GameFolder(const Remote& remote, std::string_view key) {
  const bool bare = remote.root.ends_with(':') || remote.root.ends_with('/');
  return std::format("{}{}mira-saves/v1/{}", remote.root, bare ? "" : "/", key);
}

Result<void> CheckNames(std::string_view key, std::string_view id = "x") {
  if (SafeName(key) && SafeName(id)) return {};
  return Err("remote_failed", "that can't name a folder on the remote");
}

class TempFolder {
 public:
  TempFolder() {
    std::string name = (fs::temp_directory_path() / "mira-revisions-XXXXXX").string();
    if (::mkdtemp(name.data()) != nullptr) path_ = name;
  }
  ~TempFolder() {
    std::error_code ec;
    if (!path_.empty()) fs::remove_all(path_, ec);
  }
  TempFolder(const TempFolder&) = delete;
  TempFolder& operator=(const TempFolder&) = delete;
  const fs::path& Path() const { return path_; }

 private:
  fs::path path_;
};

}  // namespace

std::uintmax_t Revision::Bytes() const {
  std::uintmax_t total = 0;
  for (const RevisionFile& file : files) total += file.bytes;
  return total;
}

std::string NewRevisionId(std::int64_t now) {
  std::random_device random;
  return std::format("{:%Y%m%dT%H%M%SZ}-{:08x}{:08x}",
                     std::chrono::sys_seconds(std::chrono::seconds(now)), random(), random());
}

json ManifestJson(const Revision& revision) {
  json files = json::array();
  for (const RevisionFile& file : revision.files)
    files.push_back({{"root", file.root},
                     {"path", file.path},
                     {"bytes", file.bytes},
                     {"mtime", file.mtime_ns},
                     {"sha256", file.sha256}});
  return {{"format", kFormat},
          {"id", revision.id},
          {"parents", revision.parents},
          {"device_id", revision.device_id},
          {"device_name", revision.device_name},
          {"game_name", revision.game_name},
          {"taken_at", revision.taken_at},
          {"files", std::move(files)}};
}

std::optional<Revision> ParseManifest(std::string_view text, std::string_view folder) {
  const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object() || core::JsonInt(doc, "format") != kFormat) return std::nullopt;
  Revision revision{.id = core::JsonString(doc, "id"),
                    .device_id = core::JsonString(doc, "device_id"),
                    .device_name = core::JsonString(doc, "device_name"),
                    .game_name = core::JsonString(doc, "game_name"),
                    .taken_at = core::JsonInt(doc, "taken_at")};
  if (revision.id != folder || !SafeName(revision.id)) return std::nullopt;
  for (const json& parent : core::JsonArray(doc, "parents")) {
    if (!parent.is_string() || !SafeName(parent.get<std::string>()) || parent == revision.id)
      return std::nullopt;
    revision.parents.push_back(parent.get<std::string>());
  }
  const json files = core::JsonArray(doc, "files");
  if (files.size() > kMaxFiles) return std::nullopt;
  for (const json& item : files) {
    if (!item.is_object()) return std::nullopt;
    const std::int64_t bytes = core::JsonInt(item, "bytes", -1);
    RevisionFile file{.root = core::JsonString(item, "root"),
                      .path = core::JsonString(item, "path"),
                      .bytes = static_cast<std::uintmax_t>(bytes),
                      .mtime_ns = core::JsonInt(item, "mtime"),
                      .sha256 = core::JsonString(item, "sha256")};
    if (bytes < 0 || bytes > (std::int64_t{1} << 40) || !std::ranges::contains(kRoots, file.root) ||
        !SafePath(file.path) || !SafeSum(file.sha256))
      return std::nullopt;
    revision.files.push_back(std::move(file));
  }
  if (!NoClash(revision.files)) return std::nullopt;
  return revision;
}

std::vector<Revision> Leaves(const std::vector<Revision>& revisions) {
  std::set<std::string> parents;
  for (const Revision& revision : revisions)
    parents.insert(revision.parents.begin(), revision.parents.end());
  std::vector<Revision> leaves;
  for (const Revision& revision : revisions)
    if (!parents.contains(revision.id)) leaves.push_back(revision);
  std::ranges::sort(leaves, [](const Revision& a, const Revision& b) {
    return std::pair(a.taken_at, a.id) > std::pair(b.taken_at, b.id);
  });
  return leaves;
}

bool SameFiles(const std::vector<RevisionFile>& a, const std::vector<RevisionFile>& b) {
  const auto sums = [](const std::vector<RevisionFile>& files) {
    std::map<std::pair<std::string, std::string>, std::string> out;
    for (const RevisionFile& file : files) out[{file.root, file.path}] = file.sha256;
    return out;
  };
  return sums(a) == sums(b);
}

std::vector<std::string> Prunable(const std::vector<Revision>& revisions, std::size_t keep) {
  const std::vector<Revision> leaves = Leaves(revisions);
  if (leaves.size() != 1) return {};
  std::map<std::string, const Revision*> by_id;
  for (const Revision& revision : revisions) by_id[revision.id] = &revision;
  // Ancestors by distance from the leaf, so a wrong clock never decides what goes.
  std::vector<std::string> order = {leaves.front().id};
  std::set<std::string> seen = {leaves.front().id};
  for (std::size_t i = 0; i < order.size(); ++i) {
    for (const std::string& parent : by_id.at(order[i])->parents)
      if (by_id.contains(parent) && seen.insert(parent).second) order.push_back(parent);
  }
  if (order.size() <= std::max<std::size_t>(keep, 1)) return {};
  // Farthest first: a delete cut short never leaves an older revision looking current.
  std::vector<std::string> out(order.begin() + std::max<std::ptrdiff_t>(keep, 1), order.end());
  std::ranges::reverse(out);
  return out;
}

Result<std::vector<std::string>> Sha256(const std::vector<fs::path>& paths) {
  constexpr std::size_t kBatch = 200;
  std::vector<std::string> sums;
  for (std::size_t at = 0; at < paths.size(); at += kBatch) {
    const std::size_t count = std::min(kBatch, paths.size() - at);
    Command command;
    command.argv = {"sha256sum", "-z", "--"};  // -z: names aren't escaped
    for (std::size_t i = 0; i < count; ++i) command.argv.push_back(paths[at + i].string());
    const auto run = runner::RunAndWait(command);
    if (!run) return std::unexpected(run.error());
    if (run->exit_code != 0) return Err("saves_failed", "couldn't read the saves to check them");
    std::size_t found = 0;
    for (const std::string_view line : strings::Split(run->output, '\0')) {
      if (line.empty()) continue;
      if (line.size() < 66 || !SafeSum(line.substr(0, 64)))
        return Err("saves_failed", "sha256sum gave no checksum");
      sums.emplace_back(line.substr(0, 64));
      ++found;
    }
    if (found != count) return Err("saves_failed", "sha256sum missed a file");
  }
  return sums;
}

Result<std::vector<Revision>> ListRevisions(const config::Config& config, const Remote& remote,
                                            std::string_view key) {
  if (auto named = CheckNames(key); !named) return std::unexpected(named.error());
  const TempFolder temp;
  if (temp.Path().empty()) return Err("remote_failed", "couldn't make a temporary folder");
  auto copied =
      RunRclone(config, remote,
                {"copy", GameFolder(remote, key), temp.Path().string(), "--include",
                 std::format("/*/{}", kManifest), "--max-depth", "2", "--max-size", "16M"});
  if (!copied) {
    if (copied.error().code == "remote_not_found") return std::vector<Revision>{};
    return std::unexpected(copied.error());
  }
  std::vector<Revision> revisions;
  for (const fs::path& folder : paths::ListDir(temp.Path())) {
    const auto text = files::ReadFile(folder / kManifest);
    auto revision =
        text ? ParseManifest(*text, folder.filename().string()) : std::optional<Revision>();
    if (revision)
      revisions.push_back(std::move(*revision));
    else
      log::Warn("the remote's revision {} of {} can't be read", folder.filename().string(), key);
  }
  return revisions;
}

Result<void> UploadRevision(const config::Config& config, const Remote& remote,
                            std::string_view key, const Revision& revision,
                            const fs::path& staged) {
  if (auto named = CheckNames(key, revision.id); !named) return named;
  // What the other computers would refuse is never uploaded.
  const std::string manifest = ManifestJson(revision).dump(2);
  if (!ParseManifest(manifest, revision.id))
    return Err(
        "remote_unsyncable",
        "These saves have names that can't be synced, such as two that differ only in case.");
  const std::string target = GameFolder(remote, key) + "/" + revision.id;
  if (!revision.files.empty()) {
    if (auto copied =
            RunRclone(config, remote, {"copy", (staged / "files").string(), target + "/files"});
        !copied)
      return std::unexpected(copied.error());
  }
  const fs::path file = staged / kManifest;
  if (auto written = WriteFileAtomic(file, manifest, "saves_failed"); !written) return written;
  if (auto copied = RunRclone(config, remote, {"copyto", file.string(), target + "/" + kManifest});
      !copied)
    return std::unexpected(copied.error());
  return {};
}

Result<void> DownloadRevision(const config::Config& config, const Remote& remote,
                              std::string_view key, const Revision& revision,
                              const fs::path& staged) {
  if (auto named = CheckNames(key, revision.id); !named) return named;
  if (revision.files.empty()) return {};
  // Only the files the manifest names, and little more than it says they hold.
  constexpr std::uintmax_t kSlack = 1 << 20;
  std::string list;
  for (const RevisionFile& file : revision.files) {
    list += file.root + "/" + file.path + "\n";
  }
  const fs::path names = staged / "names.txt";
  if (auto written = WriteFileAtomic(names, list, "saves_failed"); !written) return written;
  if (auto copied = RunRclone(
          config, remote,
          {"copy", GameFolder(remote, key) + "/" + revision.id + "/files",
           (staged / "files").string(), "--files-from-raw", names.string(), "--max-transfer",
           std::format("{}B", revision.Bytes() + kSlack), "--cutoff-mode", "hard"});
      !copied)
    return std::unexpected(copied.error());

  const Error damaged{.code = "remote_damaged",
                      .message = "The saves on the remote don't match their checksums.",
                      .hint = {}};
  std::vector<fs::path> paths;
  for (const RevisionFile& file : revision.files) {
    const fs::path path = staged / "files" / file.root / file.path;
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(path, ec)) || fs::file_size(path, ec) != file.bytes)
      return std::unexpected(damaged);
    paths.push_back(path);
  }
  const auto sums = Sha256(paths);
  if (!sums) return std::unexpected(sums.error());
  for (std::size_t i = 0; i < paths.size(); ++i)
    if ((*sums)[i] != revision.files[i].sha256) return std::unexpected(damaged);
  return {};
}

Result<void> DeleteRevision(const config::Config& config, const Remote& remote,
                            std::string_view key, std::string_view id) {
  if (auto named = CheckNames(key, id); !named) return named;
  const std::string target = GameFolder(remote, key) + "/" + std::string(id);
  // The manifest first, so a delete cut short leaves an unfinished revision.
  for (const auto& args :
       {std::vector<std::string>{"deletefile", target + "/" + kManifest}, {"purge", target}}) {
    if (auto done = RunRclone(config, remote, args);
        !done && done.error().code != "remote_not_found")
      return std::unexpected(done.error());
  }
  return {};
}

}  // namespace mira::saves::remote
