#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/Result.h"
#include "model/Types.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"

namespace mira::runner {

// Runs `winetricks --unattended <verb>` against a game's own Wine/Proton
// prefix, rather than reimplementing its crowdsourced verb definitions.
// Soft dependency on `winetricks` being on PATH: missing is reported
// clearly, never a startup failure.
//
// Resolves each runner kind's WINE binary once here (WineRunner's build path
// IS the binary; Proton/SteamRunner bundle one at files/bin/wine) rather
// than teaching winetricks-awareness to every IRunner.
// winetricks on PATH, else the copy Mira installed; empty if neither.
std::string WinetricksPath();

// Installs the latest winetricks release's script into Mira's tools folder.
Result<void> InstallWinetricks();

// The wine binary a game's runner uses (Proton's bundled one for Proton).
Result<std::filesystem::path> ResolveWineBinary(const RunnerRegistry& runners, const model::Game& game);

// Runs `wine <args>` in the game's prefix and waits for it, for setup steps
// such as regedit.
Result<ExecResult> RunWine(const RunnerRegistry& runners, const model::Game& game, const std::vector<std::string>& args);

Result<void> RunTricksVerb(const RunnerRegistry& runners, const model::Game& game, const std::string& verb,
                           const OutputFn& on_output = {});

}  // namespace mira::runner
