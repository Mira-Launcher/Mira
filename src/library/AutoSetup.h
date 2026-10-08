#pragma once

#include <filesystem>

#include "api/EventBus.h"
#include "config/Config.h"
#include "library/Detector.h"
#include "store/GameStore.h"

namespace mira::library {

// Turns one Detector::Result for a freshly-found folder into a stored,
// auto-configured model::Game, and publishes `game.added`. It does not
// provision anything: a Windows game is stored `setting_up` for the scanner
// to provision, and a native game is stored `ready`.
class AutoSetup {
public:
  AutoSetup(config::Config& config, store::GameStore& games, api::EventBus& events);

  model::Game CreateGame(const std::filesystem::path& install_path, const Detector::Result& detected);
  // A game that is one AppImage file loose in `folder` (a library root or a sorting folder in one),
  // named after the file.
  model::Game CreateAppImageGame(const std::filesystem::path& folder,
                                 const std::filesystem::path& appimage);

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::library
