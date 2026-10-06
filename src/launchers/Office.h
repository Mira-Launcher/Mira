#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "runner/RunnerRegistry.h"

// Microsoft 365 apps, installed as a launcher: one prefix set up with the
// shims from mira-winapp-shims, the Edge WebView2 runtime (sign-in) and
// Office from Microsoft's Office Deployment Tool. Mira never sees the
// account; Office signs in and checks the license itself.
namespace mira::launchers::office {

struct App {
  std::string_view ref;   // also the game id's suffix: office-<ref>
  std::string_view name;
  std::string_view exe;   // in kProgramDir
};

inline constexpr std::string_view kProgramDir = "Program Files/Microsoft Office/root/Office16";
// The Office Deployment Tool as Mira saves it; Wine settings for it are keyed by this name.
inline constexpr std::string_view kSetupFile = "office-setup.exe";

// Every app Mira imports when its exe is installed.
std::span<const App> Apps();

// The Office Deployment Tool configuration for the configured plan.
std::string Configuration(const config::Config& config);

// Installs a newer shims release into `host`'s prefix when one is out, checked
// at most every few hours. Quiet and harmless when offline.
Result<void> RefreshShims(const config::Config& config, const runner::RunnerRegistry& runners, const model::Game& host);

// Registry settings, the shim DLLs and the deployment configuration, written
// into `host`'s prefix before WebView2 and Office are installed.
Result<void> Prepare(const config::Config& config, const runner::RunnerRegistry& runners, const model::Game& host,
                     const std::filesystem::path& downloads);

}  // namespace mira::launchers::office
