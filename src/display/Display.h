#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mira::store {
class GameStore;
}

namespace mira::display {

// A monitor as Plasma's kscreen-doctor describes it.
struct Output {
  std::string name;                          // DP-3
  std::string mode;                          // the current mode's id
  std::optional<bool> hdr;                   // nullopt when the monitor can't do HDR
  std::optional<bool> wcg;                   // wide color gamut, which HDR goes with
  std::map<std::string, std::string> modes;  // id → "2560x1440@165.08"
};

// The primary monitor from `kscreen-doctor -o`'s text.
std::optional<Output> ParsePrimary(const std::string& text);
// The id of `output`'s mode for "WxH" or "WxH@Hz": the nearest refresh rate, the highest without
// one. Empty when it has no such size.
std::string FindMode(const Output& output, const std::string& wanted);

// Puts the primary monitor in a game's HDR and resolution while it runs, on Plasma, and back once
// the last game that asked exits. What to go back to is kept in mira.db, so a crash or a restart
// of mirad still restores it (RestoreLeftover).
class Switcher {
 public:
  explicit Switcher(store::GameStore& games);

  // For `game_id`, about to start. A message when it can't be done, for the game's log.
  std::string Start(const std::string& game_id, bool hdr, const std::string& mode);
  void Stop(const std::string& game_id);
  // At startup: a game's settings left behind by a mirad that didn't get to put them back.
  void RestoreLeftover();

 private:
  void Restore();

  store::GameStore& games_;
  std::mutex mutex_;
  std::vector<std::string> active_;  // games running with the monitor changed
};

}  // namespace mira::display
