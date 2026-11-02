#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/StoreTool.h"

// Epic Online Services' overlay: the in-game layer an EOS game shows Epic's own pages in. The
// first time a game signs in, Epic asks there whether the game may use the account; without the
// overlay that question never shows and the game waits at its sign-in for good (Bloons TD 6).
// Legendary downloads it from Epic; a game finds it through its prefix's registry, which Mira
// points at it before each launch.
namespace mira::epic {

// Where Mira keeps the overlay. It never requires the folder to exist.
std::filesystem::path ManagedOverlayDir(const config::Config& config);

// The overlay in ManagedOverlayDir, with the version legendary recorded for it ("1.3.7").
runner::ToolStatus DetectOverlay(const config::Config& config);

// `legendary eos-overlay install` into ManagedOverlayDir: downloads it, or updates it when Epic
// has a newer one. One runs at a time; a second waits for the first.
Result<void> InstallOverlay(const config::Config& config);

// `user_reg` (a Wine prefix's user.reg) with the EOS SDK's overlay path set to `windows_path`,
// or removed when that is empty. Anything else in it stays as it was.
std::string WithOverlayPath(std::string_view user_reg, std::string_view windows_path);

// Points `prefix` at `overlay` (a Linux path), or drops the overlay from it when `overlay` is
// empty. Leaves user.reg untouched when it already says so, and a prefix without one alone.
Result<void> SetOverlayIn(const std::filesystem::path& prefix,
                          const std::filesystem::path& overlay);

}  // namespace mira::epic
