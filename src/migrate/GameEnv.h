#pragma once

#include "config/Config.h"
#include "store/GameStore.h"

namespace mira::migrate {

// A game's environment variables were two lists: its own `env`, set over launch.env. They're one
// now: the launch.env list, a game's override replacing Settings' list. Moves what the user set in
// `env` into the game's launch.env override (from Settings' list, so the game launches with the
// same variables), keeping in `env` only what its store or launcher needs (Amazon's SDK paths,
// Battle.net's DLL overrides). Does nothing once every game is moved.
//
// Temporary: delete a release or two after one list shipped.
void MoveGameEnv(const config::Config& config, store::GameStore& games);

}  // namespace mira::migrate
