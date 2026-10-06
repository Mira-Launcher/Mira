#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "core/Result.h"
#include "model/Types.h"
#include "runner/RunnerRegistry.h"

// Windows' light/dark app setting in a prefix, kept in step with the desktop's,
// so programs that follow the system theme (Office, launchers) match it.
namespace mira::runner {

// The desktop's preference from the freedesktop settings portal: true for
// dark, false for light, nullopt when it states none or can't be read.
std::optional<bool> DesktopPrefersDark();

// `busctl` or `gdbus` output for org.freedesktop.appearance color-scheme.
std::optional<bool> ParseColorScheme(std::string_view output);

// Whether a prefix's user.reg sets apps to dark; nullopt when unset.
std::optional<bool> PrefixPrefersDark(std::string_view user_reg);

// Sets the prefix's AppsUseLightTheme and SystemUsesLightTheme to the
// desktop's preference when they differ. A no-op when the desktop has none.
Result<void> SyncWindowsTheme(const RunnerRegistry& runners, const model::Game& game);

}  // namespace mira::runner
