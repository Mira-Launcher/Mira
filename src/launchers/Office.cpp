#include "launchers/Office.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/StoreErrors.h"
#include "runner/Curl.h"
#include "runner/Exec.h"
#include "runner/Winetricks.h"

namespace mira::launchers::office {
namespace {
namespace fs = std::filesystem;

constexpr std::array kApps = {
    App{"word", "Word", "WINWORD.EXE"},       App{"excel", "Excel", "EXCEL.EXE"},
    App{"powerpoint", "PowerPoint", "POWERPNT.EXE"}, App{"outlook", "Outlook", "OUTLOOK.EXE"},
    App{"onenote", "OneNote", "ONENOTE.EXE"},  App{"access", "Access", "MSACCESS.EXE"},
    App{"publisher", "Publisher", "MSPUB.EXE"},
};

// Each shim replaces a Wine DLL and forwards to Wine's own copy, kept under a
// second name. sppc has no Wine copy to forward to.
struct Shim {
  std::string_view name;
  std::string_view wine_copy;
};
constexpr std::array kShims = {Shim{"sppc", ""}, Shim{"ole32", "ole32w"}, Shim{"uiautomationcore", "uiautomationcorew"},
                               Shim{"d2d1", "d2d1w"}, Shim{"xmllite", "xmllitew"}};

// Wine loads its own builtin in place of a file that carries this marker.
constexpr std::size_t kBuiltinMarkerAt = 64;
constexpr char kNativeMarker[16] = "Mira native DLL";

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
  reg += "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SoftwareProtectionPlatform]\n"
         "\"Version\"=\"10.0.19041.1\"\n\n";
  reg += std::format("[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Common\\Licensing\\LicensingNext]\n"
                     "\"{}\"=dword:00000002\n\n",
                     config.GetString("launchers.office.plan"));
  reg += "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Office\\ClickToRun\\Configuration]\n"
         "\"SharedComputerLicensing\"=\"0\"\n\n";

  // Wine's WinRT package manager and sandbox host are stubs that Click-to-Run
  // crashes on; without them it skips those steps. The deployment tool itself
  // needs the package manager's answer, so it keeps it.
  reg += "[HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides]\n"
         "\"appxdeploymentclient\"=\"\"\n\"hvsimanagementapi\"=\"\"\n";
  for (const Shim& shim : kShims) reg += std::format("\"{}\"=\"native,builtin\"\n", shim.name);
  reg += std::format("\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\{}\\DllOverrides]\n"
                     "\"appxdeploymentclient\"=\"builtin\"\n\n",
                     kSetupFile);

  // Sign-in through the browser flow in WebView2, not the Windows account
  // broker Wine doesn't have.
  constexpr std::string_view kIdentity =
      "\"EnableADAL\"=dword:00000001\n\"DisableADALatopWAMOverride\"=dword:00000001\n"
      "\"DisableAADWAM\"=dword:00000001\n\"DisableMSAWAM\"=dword:00000001\n";
  for (const char* root : {"HKEY_CURRENT_USER\\Software\\Microsoft", "HKEY_CURRENT_USER\\Software\\Policies\\Microsoft",
                           "HKEY_LOCAL_MACHINE\\Software\\Policies\\Microsoft"}) {
    reg += std::format("[{}\\Office\\16.0\\Common\\Identity]\n{}\n", root, kIdentity);
  }
  for (const char* app : {"word", "excel", "powerpoint", "outlook", "onenote", "access", "publisher"}) {
    reg += std::format("[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Common\\ExperimentConfigs\\"
                       "ExternalFeatureOverrides\\{}]\n"
                       "\"Microsoft.Office.Identity.TestGate.DisableBrokerForOneAuth\"=\"true\"\n"
                       "\"Microsoft.Office.Identity.FG.IsWebView2ForOneAuthEnabled\"=\"true\"\n\n",
                       app);
  }

  // Display: the desktop's scale, smoothed text, and scroll bars that stay
  // shown (Office's fading ones leave black boxes under Wine).
  reg += std::format("[HKEY_CURRENT_USER\\Control Panel\\Desktop]\n\"LogPixels\"=dword:{:08x}\n"
                     "\"FontSmoothing\"=\"2\"\n\"FontSmoothingType\"=dword:00000002\n\n",
                     DesktopDpi());
  reg += "[HKEY_CURRENT_USER\\Control Panel\\Accessibility]\n\"DynamicScrollbars\"=dword:00000000\n";
  return reg;
}

// The folder holding the shim DLLs: launchers.office.shims_dir if set, else
// the release from launchers.office.shims_url, downloaded once.
Result<fs::path> ShimsDir(const config::Config& config, const fs::path& downloads) {
  std::error_code ec;
  if (const fs::path local = config.GetPath("launchers.office.shims_dir"); !local.empty()) {
    if (!fs::is_regular_file(local / "sppc.dll", ec)) {
      return Err("office_shims_missing", std::format("no shim DLLs in {}", local.string()),
                 "Build mira-winapp-shims there, or clear launchers.office.shims_dir to download them.");
    }
    return local;
  }
  const fs::path dir = paths::UserDir() / "tools" / "mira-winapp-shims";
  const auto find = [&dir]() -> std::optional<fs::path> {
    std::error_code walk;
    for (fs::recursive_directory_iterator it(dir, walk), end; !walk && it != end; it.increment(walk)) {
      if (it->path().filename() == "sppc.dll") return it->path().parent_path();
    }
    return std::nullopt;
  };
  if (const auto found = find()) return *found;
  const fs::path archive = downloads / "mira-winapp-shims.tar.gz";
  if (auto fetched = runner::CurlDownload(config.GetString("launchers.office.shims_url"), archive); !fetched) {
    return Err("download_failed", "couldn't download the Office shims: " + fetched.error().message, kConnectionHint);
  }
  fs::create_directories(dir, ec);
  if (auto extracted = runner::Extract(archive, dir); !extracted) return std::unexpected(extracted.error());
  if (const auto found = find()) return *found;
  return Err("office_shims_missing", "the Office shims download has no DLLs in it");
}

Result<void> CopyFile(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::remove(to, ec);  // Proton links its DLLs into the prefix; never write through the link
  fs::copy_file(from, to, ec);
  if (ec) return Err("copy_failed", std::format("couldn't copy {} to {}: {}", from.string(), to.string(), ec.message()));
  return {};
}

Result<void> InstallShims(const config::Config& config, const runner::RunnerRegistry& runners, const model::Game& host,
                          const fs::path& downloads) {
  const auto shims = ShimsDir(config, downloads);
  if (!shims) return std::unexpected(shims.error());
  const auto wine = runner::ResolveWineBinary(runners, host);
  if (!wine) return std::unexpected(wine.error());
  const fs::path wine_dlls = wine->parent_path().parent_path() / "lib" / "wine" / "x86_64-windows";
  const fs::path system32 = fs::path(host.data_dir) / "drive_c" / "windows" / "system32";

  std::error_code ec;
  for (const Shim& shim : kShims) {
    const std::string dll = std::format("{}.dll", shim.name);
    if (!shim.wine_copy.empty()) {
      fs::path original = wine_dlls / dll;
      // A newer distro Wine has Direct2D fixes Office's start screen needs.
      if (shim.name == "d2d1" && fs::is_regular_file("/usr/lib/wine/x86_64-windows/d2d1.dll", ec)) {
        original = "/usr/lib/wine/x86_64-windows/d2d1.dll";
      }
      const fs::path copy = system32 / std::format("{}.dll", shim.wine_copy);
      if (auto copied = CopyFile(original, copy); !copied) return copied;
      if (shim.name == "d2d1") {
        std::fstream file(copy, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(kBuiltinMarkerAt);
        file.write(kNativeMarker, sizeof kNativeMarker);
      }
    }
    if (auto copied = CopyFile(*shims / dll, system32 / dll); !copied) return copied;
  }
  return {};
}

}  // namespace

std::span<const App> Apps() { return kApps; }

std::string Configuration(const config::Config& config) {
  // No AcceptEULA: Office shows Microsoft's license terms on first start.
  return std::format(R"(<Configuration>
  <Add OfficeClientEdition="64" Channel="Current">
    <Product ID="{}">
      <Language ID="en-us"/>
      <ExcludeApp ID="Lync"/>
      <ExcludeApp ID="Groove"/>
      <ExcludeApp ID="OneDrive"/>
      <ExcludeApp ID="Teams"/>
    </Product>
  </Add>
  <Display Level="None"/>
</Configuration>
)",
                     config.GetString("launchers.office.plan"));
}

Result<void> Prepare(const config::Config& config, const runner::RunnerRegistry& runners, const model::Game& host,
                     const fs::path& downloads) {
  std::error_code ec;
  fs::create_directories(downloads, ec);
  const fs::path reg = downloads / "office-settings.reg";
  std::ofstream(reg) << Registry(config);
  log::Info("writing Microsoft 365 settings");
  std::string windows_reg = "Z:" + reg.string();
  std::ranges::replace(windows_reg, '/', '\\');
  const auto imported = runner::RunWine(runners, host, {"regedit", "/S", windows_reg});
  if (!imported) return std::unexpected(imported.error());
  if (imported->exit_code != 0) return Err("regedit_failed", "couldn't write Microsoft 365's registry settings");

  log::Info("installing the Microsoft 365 shims");
  if (auto shims = InstallShims(config, runners, host, downloads); !shims) return shims;

  std::ofstream(downloads / "office-configuration.xml") << Configuration(config);
  return {};
}

}  // namespace mira::launchers::office
