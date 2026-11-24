#pragma once

#include <filesystem>
#include <functional>
#include <json.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ArchiveExtractor.h"
#include "library/GameSetup.h"

namespace mira::library {

// An archive found in a library or Mira folder. Only the user says whether Mira extracts it.
struct FoundArchive {
  std::filesystem::path path;  // its first volume
  std::filesystem::path root;  // the library or Mira folder it's in
  bool drop = false;           // in a Mira folder, so what's inside is sorted
  std::string taken_by;        // the app extracting it now; empty while it waits for the user
  std::vector<std::filesystem::path> outputs;  // OutputsOf, so it's listed once
};

// Kept in memory only. Events: library.archive_found {path, name, bytes, volumes, root, taken_by}
// when one appears or changes, library.archive_settled {path} when it's gone or answered for.
class FoundArchives {
 public:
  explicit FoundArchives(api::EventBus& events) : events_(events) {}

  void Offer(FoundArchive archive);
  // Takes it off the list: answered for, dealt with by another app, or no longer on disk. Nullopt
  // when it isn't there, so only one caller gets it.
  std::optional<FoundArchive> Take(const std::filesystem::path& path);
  // `appeared` is a new folder: one an archive beside it unpacks to means someone extracted it.
  void Appeared(const std::filesystem::path& appeared);
  std::vector<FoundArchive> All() const;
  std::optional<FoundArchive> Find(const std::filesystem::path& path) const;
  static nlohmann::json ToJson(const FoundArchive& archive);

 private:
  api::EventBus& events_;
  mutable std::mutex mutex_;
  std::vector<FoundArchive> archives_;
};

// The app extracting `archive` right now, as far as can be told: one that has a volume open, or a
// known extractor working on it or on one of `outputs`. Mira itself and what it runs don't count.
struct Holder {
  std::string name;
  std::uint64_t position = 0;  // how far into the volumes it is, to tell one that only sits there
  bool certain = false;        // it writes a volume, or is a known extractor
};
std::optional<Holder> HolderOf(const std::filesystem::path& archive,
                               const std::vector<std::filesystem::path>& outputs);

// The folders `archive` may unpack to beside it: its name, and what's at its top.
std::vector<std::filesystem::path> OutputsOf(const std::filesystem::path& archive);

struct ExtractOptions {
  std::string password;
  SetupChoices choices;
  bool delete_archive = true;
};
// Extracts a found archive and puts it in the library, set up with the user's choices: beside the
// archive in a library folder, in Games or Applications from a Mira folder (one with no program in
// it stays in the Mira folder to be sorted). Returns the folder.
Result<std::filesystem::path> ExtractFound(const config::Config& config,
                                           const FoundArchive& archive,
                                           const ExtractOptions& options,
                                           const std::function<void(int percent)>& on_progress);

}  // namespace mira::library
