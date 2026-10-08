#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "store/GameStore.h"

namespace mira::library {

// A folder found in a library folder that could be any of several known games whose folders are
// gone, so the scan neither adds it nor marks those games missing until someone says which it is.
struct UnclearMove {
  std::filesystem::path folder;
  std::vector<std::string> ids;
};

// The unclear moves scans found, shared by every scan so a client can list and settle them.
// Events: library.move_unclear {folder, games: [{id, name}]} when one appears, and
// library.move_settled {folder} when it's gone (settled, or no longer on disk).
class UnclearMoves {
public:
  explicit UnclearMoves(api::EventBus& events) : events_(events) {}

  // What a scan of `root` found, replacing what was known under it.
  void Replace(const std::filesystem::path& root, const std::vector<UnclearMove>& found,
               const store::GameStore& games);
  std::vector<UnclearMove> All() const;
  std::optional<UnclearMove> Find(const std::filesystem::path& folder) const;
  void Settle(const std::filesystem::path& folder);
  // The record a client gets: {folder, games: [{id, name}]}.
  static nlohmann::json ToJson(const UnclearMove& move, const store::GameStore& games);

private:
  api::EventBus& events_;
  mutable std::mutex mutex_;
  std::vector<UnclearMove> moves_;
};

}  // namespace mira::library
