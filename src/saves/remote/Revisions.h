#pragma once

#include <cstdint>
#include <filesystem>
#include <json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "saves/remote/Rclone.h"

// A game's saves on a remote: <root>/mira-saves/v1/<key>/<revision>/files/<root>/<path> and a
// manifest.json written last. A revision without a readable manifest is an unfinished upload.
namespace mira::saves::remote {

struct RevisionFile {
  std::string root;  // a logical root (saves::Roots)
  std::string path;  // inside it, '/'-separated
  std::uintmax_t bytes = 0;
  std::int64_t mtime_ns = 0;
  std::string sha256;

  bool operator==(const RevisionFile&) const = default;
};

struct Revision {
  std::string id;
  std::vector<std::string> parents{};
  std::string device_id{};
  std::string device_name{};
  std::string game_name{};
  std::int64_t taken_at = 0;
  std::vector<RevisionFile> files{};

  std::uintmax_t Bytes() const;
};

// "20261124T181200Z-" and 16 random hex digits.
std::string NewRevisionId(std::int64_t now);

nlohmann::json ManifestJson(const Revision& revision);
// Read as untrusted: nullopt for another format, an id that isn't `folder`, an unsafe path or
// root, or files that clash where case doesn't count.
std::optional<Revision> ParseManifest(std::string_view text, std::string_view folder);

// The revisions no other is built on, newest first. More than one is a conflict.
std::vector<Revision> Leaves(const std::vector<Revision>& revisions);

// Whether both hold the same saves, whatever their times.
bool SameFiles(const std::vector<RevisionFile>& a, const std::vector<RevisionFile>& b);

// The revisions pruning takes: the oldest ancestors past `keep`, only with a single leaf.
std::vector<std::string> Prunable(const std::vector<Revision>& revisions, std::size_t keep);

// The sha256 of each of `paths`, in order.
Result<std::vector<std::string>> Sha256(const std::vector<std::filesystem::path>& paths);

// A game's finished revisions; empty when the remote has none.
Result<std::vector<Revision>> ListRevisions(const config::Config& config, const Remote& remote,
                                            std::string_view key);

// Uploads `staged` (holding files/<root>/<path>) as `revision`, its manifest last.
Result<void> UploadRevision(const config::Config& config, const Remote& remote,
                            std::string_view key, const Revision& revision,
                            const std::filesystem::path& staged);

// Downloads `revision` into `staged`, an empty folder, and checks every file's size and sha256.
Result<void> DownloadRevision(const config::Config& config, const Remote& remote,
                              std::string_view key, const Revision& revision,
                              const std::filesystem::path& staged);

Result<void> DeleteRevision(const config::Config& config, const Remote& remote,
                            std::string_view key, std::string_view id);

}  // namespace mira::saves::remote
