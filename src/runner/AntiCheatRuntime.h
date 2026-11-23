#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "core/Result.h"
#include "model/Types.h"

namespace mira::config {
class Config;
}

namespace mira::runner {

// The env a Proton run of `game` needs for its anti-cheat to work outside Steam:
// PROTON_EAC_RUNTIME and PROTON_BATTLEYE_RUNTIME, each pointing at Steam's copy of Proton's runtime
// for it, else one Mira downloads once. A game uses one when the anti-cheat list names it, or its
// folder ships EasyAntiCheat or BattlEye. Empty when it uses neither, anticheat.runtimes is off,
// or it's a Steam game (Steam gives it its own). Blocks while downloading.
Result<std::map<std::string, std::string>> AntiCheatRuntimeEnv(const config::Config& config,
                                                               const model::Game& game);

}  // namespace mira::runner
