#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "store/GameStore.h"

namespace mira::library {

// Something dropped straight into a Mira folder (mira_folders) that Mira couldn't sort itself.
struct Drop {
  std::filesystem::path path;
  std::string name;
  std::string reason;  // why Mira couldn't tell
  // Where it could go: "game", "app", or an emulator's id.
  std::vector<std::string> choices;
};

// Whether `path`, at the top of a Mira folder, is the folder's own rather than a drop: a folder a
// setting names or holds, Games and the rest, or Mira's work in progress.
bool IsMiraFoldersOwn(const config::Config& config, const std::filesystem::path& path);

// Sorts what's dropped into a Mira folder, and keeps what it couldn't sort for a client to settle.
// Events: library.drop_unclear {path, name, reason, choices: [{id, label}]} when one appears, and
// library.drop_settled {path} when it's gone.
class Drops {
 public:
  Drops(config::Config& config, store::GameStore& games, api::EventBus& events)
      : config_(config), games_(games), events_(events) {}

  // A program goes into Games or Applications by what it looks like, a ROM to the one added
  // emulator that runs it; anything else Mira knows is kept to ask about. Archives are the
  // caller's to extract first. With `ask`, nothing moves without asking: for what was there
  // before mirad started, which may be the user's own.
  void Sort(const std::filesystem::path& path, bool ask = false);
  // Moves an unclear drop where `choice` (one of its choices) says.
  Result<void> Settle(const std::filesystem::path& path, const std::string& choice);
  // Drops one no longer on disk.
  void Forget(const std::filesystem::path& path);
  std::vector<Drop> All() const;
  static nlohmann::json ToJson(const Drop& drop);

 private:
  Result<void> MoveTo(const std::filesystem::path& path, const std::string& choice);
  void Ask(Drop drop);

  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
  mutable std::mutex mutex_;
  std::vector<Drop> drops_;
};

}  // namespace mira::library
