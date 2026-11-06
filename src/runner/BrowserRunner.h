#pragma once

#include <string_view>

#include "runner/IRunner.h"

namespace mira::runner {

// The browsers an HTML game runs in, by their binary's name on PATH, best first: Chromium's family
// opens a page as an app (a window with no tabs or address bar), Firefox's only as a plain window.
inline constexpr std::string_view kBrowsers =
    "helium-browser helium chromium chromium-browser ungoogled-chromium thorium-browser brave "
    "brave-browser google-chrome-stable google-chrome microsoft-edge-stable vivaldi-stable vivaldi "
    "firefox librewolf floorp waterfox zen-browser";

// True for a browser in Firefox's family, which takes Firefox's flags.
bool IsFirefoxFamily(std::string_view browser);

// The port a web game is served on, from its id, so the same on every launch and computer: browsers
// keep a page's saves by its origin, which includes the port.
int WebGamePort(const model::Game& game);

// Runs an HTML game: mira-run serves the game's folder on 127.0.0.1, and a browser opens its page
// in a window of its own, with a profile of its own in the game's data folder, so it never joins a
// browser that's already open and the session ends when that window closes.
class BrowserRunner final : public IRunner {
 public:
  explicit BrowserRunner(const config::Config& config) : config_(config) {}
  std::string kind() const override { return "browser"; }
  // Each browser on PATH, best first, so "browser:auto" takes the first.
  std::vector<model::RunnerBuild> Discover(const config::Config& config) const override;
  Result<void> Provision(const model::Game& game,
                         const std::optional<model::RunnerBuild>& build) const override;
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>& build) const override;

 private:
  const config::Config& config_;
};

}  // namespace mira::runner
