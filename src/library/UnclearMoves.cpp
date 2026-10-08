#include "library/UnclearMoves.h"

#include <algorithm>

#include "library/FolderTags.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;
}  // namespace

nlohmann::json UnclearMoves::ToJson(const UnclearMove& move, const store::GameStore& games) {
  nlohmann::json listed = nlohmann::json::array();
  for (const std::string& id : move.ids) {
    const auto game = games.Find(id);
    listed.push_back({{"id", id}, {"name", game ? game->name : id}});
  }
  return {{"folder", move.folder.string()}, {"games", std::move(listed)}};
}

void UnclearMoves::Replace(const fs::path& root, const std::vector<UnclearMove>& found,
                           const store::GameStore& games) {
  std::vector<UnclearMove> appeared;
  std::vector<fs::path> settled;
  {
    const std::lock_guard lock(mutex_);
    const auto known = [&](const fs::path& folder) {
      return std::ranges::any_of(moves_,
                                 [&](const UnclearMove& move) { return move.folder == folder; });
    };
    for (const UnclearMove& move : found) {
      if (!known(move.folder)) appeared.push_back(move);
    }
    std::erase_if(moves_, [&](const UnclearMove& move) {
      const bool gone = IsAtOrUnder(move.folder, root) &&
                        std::ranges::none_of(
                            found, [&](const UnclearMove& f) { return f.folder == move.folder; });
      if (gone) settled.push_back(move.folder);
      return gone || IsAtOrUnder(move.folder, root);
    });
    moves_.insert(moves_.end(), found.begin(), found.end());
  }
  for (const UnclearMove& move : appeared)
    events_.Publish("library.move_unclear", ToJson(move, games));
  for (const fs::path& folder : settled)
    events_.Publish("library.move_settled", {{"folder", folder.string()}});
}

std::vector<UnclearMove> UnclearMoves::All() const {
  const std::lock_guard lock(mutex_);
  return moves_;
}

std::optional<UnclearMove> UnclearMoves::Find(const fs::path& folder) const {
  const std::lock_guard lock(mutex_);
  const auto found = std::ranges::find_if(moves_, [&](const UnclearMove& move) {
    return move.folder.lexically_normal() == folder.lexically_normal();
  });
  return found == moves_.end() ? std::nullopt : std::optional(*found);
}

void UnclearMoves::Settle(const fs::path& folder) {
  {
    const std::lock_guard lock(mutex_);
    std::erase_if(moves_, [&](const UnclearMove& move) {
      return move.folder.lexically_normal() == folder.lexically_normal();
    });
  }
  events_.Publish("library.move_settled", {{"folder", folder.string()}});
}

}  // namespace mira::library
