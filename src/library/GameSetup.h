#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::library {

// What the user said about a new game; what's left out stays as Mira guessed it.
struct SetupChoices {
  std::optional<std::string> name;
  std::optional<std::string> exe_path;      // relative to the game's folder, or absolute
  std::optional<bool> installer;            // the program installs the game rather than starts it
  std::optional<bool> app;                  // an application, not a game
  std::optional<std::int64_t> steam_appid;  // which title it is; 0 when it's none on Steam
};

// The choices in a request body, or why they aren't valid.
Result<SetupChoices> ParseSetupChoices(const nlohmann::json& body);

// Puts `choices` on a game that isn't set up. It becomes ready, setting_up when its runner still
// has to be, or stays not set up until its installer is run. Err no_executable without a program.
Result<void> ConfirmSetup(const config::Config& config, const store::GameStore& games,
                          model::Game& game, const SetupChoices& choices);

// The name, kind and title in `choices`, without setting the game up.
void ApplyChoices(model::Game& game, const SetupChoices& choices);

// The user answered for a game before it was in the library: a scan that finds it in `folder` adds
// it confirmed. Lasts until it's found or mirad restarts.
void ConfirmWhenFound(const std::filesystem::path& folder, SetupChoices choices);
std::optional<SetupChoices> TakeConfirmed(const std::filesystem::path& folder);

// Ready, or setting_up when its runner still has to be.
model::GameStatus RunnableStatus(const model::Game& game);

}  // namespace mira::library
