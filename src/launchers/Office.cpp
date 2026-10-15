#include "launchers/Office.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/StoreErrors.h"
#include "launchers/Shims.h"
#include "runner/Curl.h"
#include "runner/Exec.h"
#include "runner/Winetricks.h"

namespace mira::launchers::office {
namespace {
namespace fs = std::filesystem;

constexpr std::array kApps = {
    App{"word", "Word", "WINWORD.EXE",
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document;application/"
        "msword;"
        "application/vnd.openxmlformats-officedocument.wordprocessingml.template;"
        "application/vnd.ms-word.document.macroEnabled.12;application/rtf;"},
    App{"excel", "Excel", "EXCEL.EXE",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet;application/"
        "vnd.ms-excel;"
        "application/vnd.openxmlformats-officedocument.spreadsheetml.template;"
        "application/vnd.ms-excel.sheet.macroEnabled.12;text/csv;"},
    App{"powerpoint", "PowerPoint", "POWERPNT.EXE",
        "application/vnd.openxmlformats-officedocument.presentationml.presentation;application/"
        "vnd.ms-powerpoint;"
        "application/vnd.openxmlformats-officedocument.presentationml.slideshow;"
        "application/vnd.ms-powerpoint.presentation.macroEnabled.12;"},
    App{"outlook", "Outlook", "OUTLOOK.EXE", "application/vnd.ms-outlook;"},
    App{"onenote", "OneNote", "ONENOTE.EXE", "application/onenote;"},
    App{"access", "Access", "MSACCESS.EXE", "application/vnd.ms-access;application/x-msaccess;"},
    App{"publisher", "Publisher", "MSPUB.EXE",
        "application/vnd.ms-publisher;application/x-mspublisher;"},
};

// sppc has no Wine copy to forward to, and neither has qmgr: it is a whole BITS service of its own
// (Wine's cannot take the file ranges Office downloads with).
using shims::Shim;
constexpr std::array kShims = {Shim{"sppc", ""},
                               Shim{"ole32", "ole32w"},
                               Shim{"uiautomationcore", "uiautomationcorew"},
                               Shim{"d2d1", "d2d1w"},
                               Shim{"xmllite", "xmllitew"},
                               Shim{"qmgr", ""}};

// In the prefix: the shims release installed there.
constexpr std::string_view kShimsMarker = "mira-shims-release";

// Xft.dpi from the X resources, so Office matches the desktop's scale; 96 if unset.
int DesktopDpi() {
  Command command;
  command.argv = {"xrdb", "-query"};
  const auto result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) return 96;
  const std::size_t at = result->output.find("Xft.dpi:");
  if (at == std::string::npos) return 96;
  const int dpi = std::atoi(result->output.c_str() + at + 8);
  return dpi >= 96 && dpi <= 480 ? dpi : 96;
}

std::string Registry(const config::Config& config) {
  std::string reg = "Windows Registry Editor Version 5.00\n\n";
  // The installer looks for the Software Protection Platform; our sppc shim
  // answers that no license is installed, and Office then uses its
  // subscription licensing, signed in to the user's own account.
  reg +=
      "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows "
      "NT\\CurrentVersion\\SoftwareProtectionPlatform]\n"
      "\"Version\"=\"10.0.19041.1\"\n\n";
  reg += std::format(
      "[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Common\\Licensing\\LicensingNext]\n"
      "\"{}\"=dword:00000002\n\n",
      config.GetString("launchers.office.plan"));
  reg +=
      "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Office\\ClickToRun\\Configuration]\n"
      "\"SharedComputerLicensing\"=\"0\"\n\n";

  // Wine's WinRT package manager and sandbox host are stubs that Click-to-Run
  // crashes on; without them it skips those steps. The deployment tool itself
  // needs the package manager's answer, so it keeps it.
  reg +=
      "[HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides]\n"
      "\"appxdeploymentclient\"=\"\"\n\"hvsimanagementapi\"=\"\"\n";
  for (const Shim& shim : kShims) reg += std::format("\"{}\"=\"native,builtin\"\n", shim.name);
  reg += std::format(
      "\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\{}\\DllOverrides]\n"
      "\"appxdeploymentclient\"=\"builtin\"\n\n",
      kSetupFile);
  // WebView2, which signs in, presents through a composition swapchain only Wine's own dxgi has. It
  // stays off the shims too: its browser quit before loading the sign-in page.
  reg +=
      "[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\msedgewebview2.exe\\DllOverrides]\n"
      "\"dxgi\"=\"builtin\"\n\"d3d11\"=\"builtin\"\n";
  for (const Shim& shim : kShims) reg += std::format("\"{}\"=\"builtin\"\n", shim.name);
  reg += "\n";

  // Sign-in through the browser flow in WebView2, not the Windows account
  // broker Wine doesn't have.
  constexpr std::string_view kIdentity =
      "\"EnableADAL\"=dword:00000001\n\"DisableADALatopWAMOverride\"=dword:00000001\n"
      "\"DisableAADWAM\"=dword:00000001\n\"DisableMSAWAM\"=dword:00000001\n";
  for (const char* root : {"HKEY_CURRENT_USER\\Software\\Microsoft",
                           "HKEY_CURRENT_USER\\Software\\Policies\\Microsoft",
                           "HKEY_LOCAL_MACHINE\\Software\\Policies\\Microsoft"}) {
    reg += std::format("[{}\\Office\\16.0\\Common\\Identity]\n{}\n", root, kIdentity);
  }
  for (const App& app : kApps) {
    reg += std::format(
        "[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Common\\ExperimentConfigs\\"
        "ExternalFeatureOverrides\\{}]\n"
        "\"Microsoft.Office.Identity.TestGate.DisableBrokerForOneAuth\"=\"true\"\n"
        "\"Microsoft.Office.Identity.FG.IsWebView2ForOneAuthEnabled\"=\"true\"\n\n",
        app.ref);
  }

  // Outlook's very first start, with no UI language recorded yet, fails to
  // open its profile ("Cannot start Microsoft Outlook"). The install is en-us.
  reg +=
      "[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Outlook]\n"
      "\"LastUILanguage\"=dword:00000409\n\n";

  // OneNote treats every Windows 10 version as a server, then wants Desktop
  // Experience: this class key or a Win32_ServerFeature row Wine's WMI lacks.
  reg +=
      "[HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\{937C1A34-151D-4610-9CA6-A8CC9BDB5D83}]\n"
      "@=\"Desktop Experience\"\n\n";

  // Display: the desktop's scale, smoothed text, and scroll bars that stay
  // shown (Office's fading ones leave black boxes under Wine).
  reg += std::format(
      "[HKEY_CURRENT_USER\\Control Panel\\Desktop]\n\"LogPixels\"=dword:{:08x}\n"
      "\"FontSmoothing\"=\"2\"\n\"FontSmoothingType\"=dword:00000002\n\n",
      DesktopDpi());
  reg +=
      "[HKEY_CURRENT_USER\\Control Panel\\Accessibility]\n\"DynamicScrollbars\"=dword:00000000\n";
  return reg;
}

}  // namespace

std::span<const App> Apps() {
  return kApps;
}

std::string Configuration(const config::Config& config, std::span<const std::string> apps) {
  if (apps.empty())
    return "<Configuration>\n  <Remove All=\"TRUE\"/>\n  <Display "
           "Level=\"None\"/>\n</Configuration>\n";
  std::string excluded;
  for (const App& app : kApps) {
    if (!std::ranges::contains(apps, app.ref))
      excluded += std::format("      <ExcludeApp ID=\"{}\"/>\n", app.name);
  }
  // No AcceptEULA: Office shows Microsoft's license terms on first start.
  return std::format(R"(<Configuration>
  <Add OfficeClientEdition="64" Channel="Current">
    <Product ID="{}">
      <Language ID="en-us"/>
      <ExcludeApp ID="Lync"/>
      <ExcludeApp ID="Groove"/>
      <ExcludeApp ID="OneDrive"/>
      <ExcludeApp ID="Teams"/>
{}    </Product>
  </Add>
  <Display Level="None"/>
</Configuration>
)",
                     config.GetString("launchers.office.plan"), excluded);
}

// Writes Registry() into the prefix.
Result<void> ImportRegistry(const config::Config& config, const runner::RunnerRegistry& runners,
                            const model::Game& host, const fs::path& downloads) {
  std::error_code ec;
  fs::create_directories(downloads, ec);
  const fs::path reg = downloads / "office-settings.reg";
  std::ofstream(reg) << Registry(config);
  std::string windows_reg = "Z:" + reg.string();
  std::ranges::replace(windows_reg, '/', '\\');
  const auto imported = runner::RunWine(runners, host, {"regedit", "/S", windows_reg});
  if (!imported) return std::unexpected(imported.error());
  if (imported->exit_code != 0)
    return Err("regedit_failed", "couldn't write Microsoft 365's registry settings");
  return {};
}

Result<void> RefreshShims(const config::Config& config, const runner::RunnerRegistry& runners,
                          const model::Game& host) {
  // At most one check every few hours; a launch never waits on it twice.
  static std::mutex mutex;
  static std::chrono::steady_clock::time_point last;
  std::lock_guard lock(mutex);
  const auto now = std::chrono::steady_clock::now();
  if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::hours(6))
    return {};

  const fs::path marker = fs::path(host.data_dir) / kShimsMarker;
  const fs::path downloads = paths::UserDir() / "downloads";
  std::string version;
  auto dir = shims::Dir(config, downloads, &version);
  if (!dir) return std::unexpected(dir.error());  // asked again next launch
  if ((!version.empty() && version == shims::ReadLine(marker)) ||
      (version.empty() && fs::exists(marker))) {
    last = now;  // without a version, shims_dir: Prepare installed them
    return {};
  }
  log::Info("updating the Microsoft 365 shims to {}",
            version.empty() ? "the local build" : version);
  // A new release can come with new settings.
  if (auto imported = ImportRegistry(config, runners, host, downloads); !imported) return imported;
  if (auto installed = shims::Install(config, runners, host, downloads, kShims, &version);
      !installed)
    return installed;
  std::ofstream(marker) << version << "\n";
  last = now;
  return {};
}

std::optional<int> InstallPercent(const fs::path& prefix, fs::file_time_type since) {
  constexpr std::string_view kMarker = "UpdateScenarioProgress";
  constexpr std::streamoff kTail = 256 * 1024;
  std::error_code ec;
  fs::file_time_type newest_time = since;  // a log has to be at least this new
  std::optional<int> percent;
  for (const auto& user : fs::directory_iterator(prefix / "drive_c" / "users", ec)) {
    for (const auto& entry : fs::directory_iterator(user.path() / "AppData/Local/Temp", ec)) {
      std::error_code entry_ec;
      if (entry.path().extension() != ".log" || !entry.is_regular_file(entry_ec)) continue;
      const auto written = entry.last_write_time(entry_ec);
      if (entry_ec || written < newest_time) continue;
      // The tail only: these logs run to megabytes. Some are UTF-16, so drop the NULs and read it
      // as text.
      std::ifstream in(entry.path(), std::ios::binary);
      in.seekg(0, std::ios::end);
      const std::streamoff size = in.tellg();
      in.seekg(size > kTail ? size - kTail : 0);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      std::erase(text, '\0');
      // "ScenarioController::UpdateScenarioProgress - {guid}=11"
      const std::size_t at = text.rfind(kMarker);
      if (at == std::string::npos) continue;
      const std::size_t equals = text.find('=', at);
      if (equals == std::string::npos) continue;
      const int value = std::atoi(text.c_str() + equals + 1);
      if (value < 0 || value > 100) continue;
      newest_time = written;
      percent = value;
    }
  }
  return percent;
}

std::uint64_t InstallBytes(const fs::path& prefix) {
  std::uint64_t total = 0;
  std::error_code ec;
  for (const char* folder : {"ProgramData/Microsoft/ClickToRun", "Program Files/Microsoft Office",
                             "Program Files/Common Files/Microsoft Shared/ClickToRun"}) {
    for (fs::recursive_directory_iterator
             it(prefix / "drive_c" / folder, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
      struct stat info;
      if (it->is_regular_file(ec) && ::stat(it->path().c_str(), &info) == 0)
        total += static_cast<std::uint64_t>(info.st_blocks) * 512;
    }
    ec.clear();
  }
  return total;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> DownloadProgress(const fs::path& prefix) {
  std::ifstream in(prefix / "drive_c" / "windows" / "temp" / "mira-bits.txt");
  long long done = 0;
  long long total = 0;
  if (!(in >> done >> total) || total <= 0 || done < 0) return std::nullopt;
  return std::pair{static_cast<std::uint64_t>(done), static_cast<std::uint64_t>(total)};
}

Result<void> Prepare(const config::Config& config, const runner::RunnerRegistry& runners,
                     const model::Game& host, const fs::path& downloads) {
  log::Info("writing Microsoft 365 settings");
  if (auto imported = ImportRegistry(config, runners, host, downloads); !imported) return imported;

  log::Info("installing the Microsoft 365 shims");
  std::string version;
  if (auto installed = shims::Install(config, runners, host, downloads, kShims, &version);
      !installed)
    return installed;
  std::ofstream(fs::path(host.data_dir) / kShimsMarker) << version << "\n";

  return {};
}

}  // namespace mira::launchers::office
