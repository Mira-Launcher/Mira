#include "display/Display.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <json.hpp>
#include <regex>
#include <sstream>
#include <tuple>

#include "core/Command.h"
#include "core/Log.h"
#include "runner/Exec.h"
#include "store/GameStore.h"

namespace mira::display {
namespace {

constexpr const char* kRestoreKey = "display_restore";

std::optional<bool> State(const std::string& value) {
  if (value == "enabled") return true;
  if (value == "disabled") return false;
  return std::nullopt;  // incapable
}

// Plasma on Wayland, where kscreen-doctor sets modes and HDR.
bool Supported() {
  const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
  return desktop != nullptr && std::string(desktop).find("KDE") != std::string::npos &&
         std::getenv("WAYLAND_DISPLAY") != nullptr && runner::FindOnPath("kscreen-doctor");
}

Result<std::string> Doctor(const std::vector<std::string>& args) {
  Command command;
  command.argv = {"kscreen-doctor"};
  command.argv.insert(command.argv.end(), args.begin(), args.end());
  auto ran = runner::RunAndWait(command);
  if (!ran) return std::unexpected(ran.error());
  if (ran->exit_code != 0)
    return Err("display_failed", std::format("kscreen-doctor failed: {}", ran->output));
  return ran->output;
}

std::vector<std::string> Settings(const std::string& output, const std::string& mode,
                                  std::optional<bool> hdr, std::optional<bool> wcg) {
  std::vector<std::string> args;
  const std::string prefix = "output." + output + ".";
  if (!mode.empty()) args.push_back(prefix + "mode." + mode);
  if (hdr) args.push_back(prefix + (*hdr ? "hdr.enable" : "hdr.disable"));
  if (wcg) args.push_back(prefix + (*wcg ? "wcg.enable" : "wcg.disable"));
  return args;
}

}  // namespace

std::optional<Output> ParsePrimary(const std::string& raw) {
  static const std::regex kColor("\x1b\\[[0-9;]*m");
  std::istringstream lines(std::regex_replace(raw, kColor, ""));
  std::optional<Output> current, primary;
  bool is_primary = false;
  const auto done = [&] {
    if (current && is_primary && !primary) primary = std::move(current);
    current.reset();
    is_primary = false;
  };
  for (std::string line; std::getline(lines, line);) {
    if (line.starts_with("Output: ")) {
      done();
      std::istringstream fields(line.substr(8));
      std::string id;
      current = Output{};
      fields >> id >> current->name;
      continue;
    }
    if (!current) continue;
    line.erase(0, line.find_first_not_of(" \t"));
    if (line == "priority 1") is_primary = true;
    if (line.starts_with("HDR: ")) current->hdr = State(line.substr(5));
    if (line.starts_with("Wide Color Gamut: ")) current->wcg = State(line.substr(18));
    if (line.starts_with("Modes: ")) {
      std::istringstream modes(line.substr(7));
      for (std::string mode; modes >> mode;) {
        const std::size_t colon = mode.find(':');
        if (colon == std::string::npos) continue;
        const std::string id = mode.substr(0, colon);
        std::string name = mode.substr(colon + 1);
        if (name.find('*') != std::string::npos) current->mode = id;
        std::erase(name, '*');
        std::erase(name, '!');
        current->modes[id] = name;
      }
    }
  }
  done();
  return primary;
}

std::string FindMode(const Output& output, const std::string& wanted) {
  static const std::regex kWanted(R"((\d+)x(\d+)(?:@(\d+(?:\.\d+)?))?)");
  std::smatch want;
  if (!std::regex_match(wanted, want, kWanted)) return {};
  const std::string size = want[1].str() + "x" + want[2].str();
  const double hz = want[3].matched ? std::stod(want[3].str()) : 0;
  std::string best;
  double best_hz = 0;
  for (const auto& [id, name] : output.modes) {
    const std::size_t at = name.find('@');
    if (at == std::string::npos || name.substr(0, at) != size) continue;
    const double rate = std::stod(name.substr(at + 1));
    const bool better =
        best.empty() || (hz > 0 ? std::abs(rate - hz) < std::abs(best_hz - hz) : rate > best_hz);
    if (better) {
      best = id;
      best_hz = rate;
    }
  }
  return best;
}

std::vector<std::string> ModeChoices(const Output& output) {
  struct Mode {
    int width, height, hz;
  };
  std::vector<Mode> modes;
  static const std::regex kMode(R"((\d+)x(\d+)@(\d+(?:\.\d+)?))");
  for (const auto& [id, name] : output.modes) {
    std::smatch match;
    if (!std::regex_match(name, match, kMode)) continue;
    modes.push_back({std::stoi(match[1]), std::stoi(match[2]),
                     static_cast<int>(std::lround(std::stod(match[3])))});
  }
  std::ranges::sort(modes, [](const Mode& a, const Mode& b) {
    return std::tie(b.width, b.height, b.hz) < std::tie(a.width, a.height, a.hz);
  });
  std::vector<std::string> choices;
  for (const Mode& mode : modes) {
    std::string choice = std::format("{}x{}@{}", mode.width, mode.height, mode.hz);
    if (std::ranges::find(choices, choice) == choices.end()) choices.push_back(std::move(choice));
  }
  return choices;
}

std::optional<Output> Primary() {
  if (!Supported()) return std::nullopt;
  auto listed = Doctor({"-o"});
  return listed ? ParsePrimary(*listed) : std::nullopt;
}

Switcher::Switcher(store::GameStore& games) : games_(games) {}

std::string Switcher::Start(const std::string& game_id, bool hdr, const std::string& mode) {
  if (!hdr && mode.empty()) return {};
  const std::lock_guard lock(mutex_);
  // Another game already changed it: that one's settings stay until the last exits.
  if (!active_.empty()) {
    active_.push_back(game_id);
    return {};
  }
  if (!Supported()) return "HDR and resolution switching need Plasma on Wayland";
  auto listed = Doctor({"-o"});
  if (!listed) return listed.error().message;
  const std::optional<Output> output = ParsePrimary(*listed);
  if (!output) return "Mira couldn't find the primary monitor";

  std::string message;
  std::optional<bool> want_hdr, want_wcg;
  if (hdr && !output->hdr) {
    message = output->name + " can't do HDR, so it stays off";
  } else if (hdr && !*output->hdr) {
    want_hdr = true;
    if (output->wcg && !*output->wcg) want_wcg = true;
  }
  std::string want_mode;
  if (!mode.empty()) {
    want_mode = FindMode(*output, mode);
    if (want_mode.empty()) message = output->name + " has no " + mode + " mode";
    if (want_mode == output->mode) want_mode.clear();
  }
  const std::vector<std::string> args = Settings(output->name, want_mode, want_hdr, want_wcg);
  if (args.empty()) return message;

  // A leftover a restart couldn't put back still holds the monitor's own settings: kept.
  if (!games_.Secret(kRestoreKey).empty()) {
    if (auto applied = Doctor(args); !applied) return applied.error().message;
    log::Info("{} set {} for {}", output->name, nlohmann::json(args).dump(), game_id);
    active_.push_back(game_id);
    return message;
  }
  // Saved first: if mirad dies between the two, the next one still puts it back.
  const nlohmann::json restore = {{"output", output->name},
                                  {"mode", want_mode.empty() ? "" : output->mode},
                                  {"hdr", want_hdr ? nlohmann::json(false) : nlohmann::json()},
                                  {"wcg", want_wcg ? nlohmann::json(false) : nlohmann::json()}};
  if (auto saved = games_.SetSecret(kRestoreKey, restore.dump()); !saved)
    return saved.error().message;
  if (auto applied = Doctor(args); !applied) {
    [[maybe_unused]] auto cleared = games_.SetSecret(kRestoreKey, "");
    return applied.error().message;
  }
  log::Info("{} set {} for {}", output->name, nlohmann::json(args).dump(), game_id);
  active_.push_back(game_id);
  return message;
}

void Switcher::Stop(const std::string& game_id) {
  const std::lock_guard lock(mutex_);
  const auto found = std::ranges::find(active_, game_id);
  if (found == active_.end()) return;
  active_.erase(found);
  if (active_.empty()) Restore();
}

void Switcher::RestoreLeftover() {
  const std::lock_guard lock(mutex_);
  Restore();
}

void Switcher::Restore() {
  const nlohmann::json saved = nlohmann::json::parse(games_.Secret(kRestoreKey), nullptr, false);
  if (!saved.is_object()) return;
  const auto flag = [&](const char* key) -> std::optional<bool> {
    return saved.contains(key) && saved[key].is_boolean() ? std::optional(saved[key].get<bool>())
                                                          : std::nullopt;
  };
  const std::vector<std::string> args =
      Settings(saved.value("output", std::string()), saved.value("mode", std::string()),
               flag("hdr"), flag("wcg"));
  if (!args.empty() && !Supported()) return;  // kept for a session where it can be put back
  if (!args.empty()) {
    if (auto applied = Doctor(args); !applied) {
      log::Warn("couldn't put the monitor back: {}", applied.error().message);
      return;  // kept, to try again at the next start
    }
    log::Info("put {} back after the game", saved.value("output", std::string()));
  }
  [[maybe_unused]] auto cleared = games_.SetSecret(kRestoreKey, "");
}

}  // namespace mira::display
