#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <utility>
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

// How far the install has got, 0-100, from the log Click-to-Run writes in the
// prefix's Temp folder (the installer itself prints nothing). Only logs written
// since `since` count, so an earlier install's 100 isn't read as this one's.
// Empty until Office has reported a figure.
std::optional<int> InstallPercent(const std::filesystem::path& prefix, std::filesystem::file_time_type since);

// Bytes on disk under Office's download and install folders in `prefix`: sampled over time, the speed of
// the download and then of the unpacking. Counts blocks, not file sizes, as files are sized before they are filled.
std::uint64_t InstallBytes(const std::filesystem::path& prefix);

// How much of what Office has asked to download has arrived, as the BITS shim reports it (done, total): Office
// itself says nothing while it downloads. Empty before the shim has been given a file.
std::optional<std::pair<std::uint64_t, std::uint64_t>> DownloadProgress(const std::filesystem::path& prefix);

// Registry settings, the shim DLLs and the deployment configuration, written
// into `host`'s prefix before WebView2 and Office are installed.
Result<void> Prepare(const config::Config& config, const runner::RunnerRegistry& runners, const model::Game& host,
                     const std::filesystem::path& downloads);

}  // namespace mira::launchers::office
