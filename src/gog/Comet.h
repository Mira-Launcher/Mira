#pragma once

#include <filesystem>
#include <string>

#include "config/Config.h"
#include "core/Command.h"
#include "core/Result.h"
#include "runner/Downloader.h"
#include "runner/StoreTool.h"

// comet (github.com/imLinguin/comet) stands in for GOG Galaxy's communication service: GOG games
// that use the Galaxy SDK connect to it on localhost and it posts their achievement unlocks to
// GOG. One copy serves every running GOG game.
namespace mira::gog {

std::filesystem::path ManagedCometPath(const config::Config& config);

runner::ToolStatus DetectComet(const config::Config& config);

Result<void> InstallCometBinary(const config::Config& config, const runner::ReleaseAsset& asset);

// The account's GOG user name, for comet to hand to games; the user id when GOG doesn't say.
// Asked of GOG once per account and remembered while mirad runs.
std::string Username(const config::Config& config);

// How to start comet with the account gogdl signed in to. The tokens stay off its command line:
// comet's --from-heroic reads $XDG_CONFIG_PATH/heroic/gog_store/auth.json, which is gogdl's own
// format, so that path is a link to Mira's gogdl sign-in.
Result<Command> CometCommand(const config::Config& config);

}  // namespace mira::gog
