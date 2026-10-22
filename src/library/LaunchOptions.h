#pragma once

#include <json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "model/Types.h"

// A game's other ways to start and the programs that run beside it: a mod manager, a deck tracker.
// Kept in game.launch as {"options": [...], "default": id, "only": bool}.
namespace mira::library {

enum class LaunchMode {
  Play,       // starts instead of the game's own program, tracked as the game
  Alongside,  // starts with every launch and is closed when the game exits
  Tool,       // opened by itself, not tracked
};

struct LaunchOption {
  std::string id;
  std::string name;
  LaunchMode mode = LaunchMode::Play;
  std::string exe;  // absolute, or relative to install_path
  std::string args;
  bool native = false;  // a Linux program, not run with the game's runner
  // Whether a play option gets the store's sign-in (Epic's); unset decides by SignsIn.
  std::optional<bool> sign_in;
};

// The id that names the game's own way to start.
inline constexpr std::string_view kGameLaunch = "game";

std::vector<LaunchOption> LaunchOptions(const model::Game& game);
std::optional<LaunchOption> FindLaunchOption(const model::Game& game, std::string_view id);
// What Play starts with no choice made: the default option, or nullopt for the game's own way.
std::optional<LaunchOption> DefaultLaunch(const model::Game& game);
// Whether `option` is started with the store's sign-in: as set, else when its program is inside
// the game's install folder (another program of the game), not another one such as a mod manager.
bool SignsIn(const model::Game& game, const LaunchOption& option);

// Why `launch` can't be a game's launch, or nullopt.
std::optional<std::string> LaunchProblem(const nlohmann::json& launch);
// `launch` with an id on every option, made from its name.
nlohmann::json WithLaunchIds(nlohmann::json launch);

// `text` with {game_dir}, {game_exe} and {prefix} filled in, as Wine paths when `wine_paths`.
std::string FillLaunchPlaceholders(std::string_view text, const model::Game& game, bool wine_paths);

}  // namespace mira::library
