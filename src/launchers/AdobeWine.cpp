#include "launchers/AdobeWine.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <optional>
#include <tuple>
#include <vector>

#include "core/Paths.h"
#include "core/Strings.h"
#include "runner/Curl.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "runner/Winetricks.h"

namespace mira::launchers::adobewine {
namespace {
namespace fs = std::filesystem;

// In a build: AdobeWine's own DLLs and tools (its lib folder), beside Wine's.
constexpr std::string_view kExtras = "adobewine";

// The newest build here whose name starts with `prefix`.
std::optional<std::string> NewestHere(std::string_view prefix) {
  std::error_code ec;
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(BuildsDir(), ec)) {
    const std::string name = entry.path().filename().string();
    if (name.starts_with(prefix) && fs::exists(entry.path() / "bin" / "wine", ec)) names.push_back(name);
  }
  if (names.empty()) return std::nullopt;
  return *std::ranges::max_element(names);
}

// The release unpacks to opt/adobewine with Wine in runtime/; the build keeps that runtime, plus its
// lib folder and licence.
Result<void> Download(const runner::ReleaseAsset& release, const std::string& name) {
  const fs::path archive = paths::UserDir() / "downloads" / release.asset_name;
  const fs::path staging = BuildsDir() / (".unpack-" + name);
  std::error_code ec;
  fs::create_directories(archive.parent_path(), ec);
  if (auto fetched = runner::CurlDownload(release.download_url, archive); !fetched) {
    return Err("download_failed", "couldn't download AdobeWine: " + fetched.error().message);
  }
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  if (auto extracted = runner::Extract(archive, staging); !extracted) return extracted;
  fs::remove(archive, ec);
  const fs::path root = staging / "opt" / "adobewine";
  if (!fs::exists(root / "runtime" / "bin" / "wine", ec)) {
    fs::remove_all(staging, ec);
    return Err("download_failed", std::format("{} has no Wine in it", release.asset_name));
  }
  fs::rename(root / "lib", root / "runtime" / kExtras, ec);
  fs::copy_file(root / "LICENSE", root / "runtime" / "LICENSE", fs::copy_options::overwrite_existing, ec);
  std::error_code moved;
  fs::rename(root / "runtime", BuildsDir() / name, moved);
  fs::remove_all(staging, ec);
  if (moved) return Err("download_failed", std::format("couldn't unpack AdobeWine: {}", moved.message()));
  return {};
}

}  // namespace

fs::path BuildsDir() { return paths::UserDir() / "tools" / "adobewine"; }

Result<std::string> Runtime(const config::Config& config, const std::function<void(const std::string&)>& say) {
  std::string repo(kRepo);
  std::string tag;
  if (const std::string url = config.GetString("launchers.adobe.adobewine_url"); !url.empty()) {
    const auto parsed = strings::ParseGithubUrl(url);
    if (!parsed) return Err("invalid_value", std::format("{} isn't a GitHub repository or release link", url));
    std::tie(repo, tag) = *parsed;
  }
  // Another repository's builds keep its owner in their name, apart from le-birnes's.
  const std::string prefix = repo == kRepo ? "" : repo.substr(0, repo.find('/')) + "-";
  const runner::RunnerFamily source{.id = "adobewine", .kind = "wine", .label = "AdobeWine", .repo = repo,
                                    .asset_pattern = "adobewine-*-x86_64.tar.zst", .build_pattern = "", .exclude = {}};
  const auto releases = runner::ListFamilyReleases(source);
  if (releases && !releases->empty()) {
    const auto release = tag.empty() ? releases->begin() : std::ranges::find(*releases, tag, &runner::ReleaseAsset::tag);
    if (release == releases->end()) {
      return Err("runner_build_not_found", std::format("github.com/{} has no release {} with an AdobeWine build", repo, tag));
    }
    const std::string name = prefix + runner::ReleaseName("wine", *release);
    std::error_code ec;
    if (!fs::exists(BuildsDir() / name / "bin" / "wine", ec)) {
      say(std::format("Downloading {} from github.com/{}", name, repo));
      if (auto downloaded = Download(*release, name); !downloaded) return std::unexpected(downloaded.error());
    }
    return "wine:" + name;
  }
  if (const auto here = NewestHere(prefix.empty() ? "adobewine-" : prefix)) return "wine:" + *here;
  if (!releases) return std::unexpected(releases.error());
  return Err("runner_build_not_found", std::format("github.com/{} has no release to download", repo));
}

Result<void> Prepare(const runner::RunnerRegistry& runners, const model::Game& host, const fs::path& downloads) {
  std::error_code ec;
  fs::create_directories(downloads, ec);
  const fs::path reg = downloads / "adobe-settings.reg";
  // Photoshop divides by zero at start on Windows 11's build; WebView2 presents through a composition
  // swapchain only Wine's own dxgi has; a title bar resizes Adobe's maximised windows and leaves panels undrawn.
  std::ofstream(reg) << "Windows Registry Editor Version 5.00\n\n"
                        "[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\Photoshop.exe]\n\"Version\"=\"win10\"\n\n"
                        "[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\msedgewebview2.exe\\DllOverrides]\n"
                        "\"dxgi\"=\"builtin\"\n\"d3d11\"=\"builtin\"\n\n"
                        "[HKEY_CURRENT_USER\\Software\\Wine\\Drivers]\n\"Graphics\"=\"x11\"\n\n"
                        "[HKEY_CURRENT_USER\\Software\\Wine\\X11 Driver]\n\"Decorated\"=\"N\"\n\n"
                        "[HKEY_CURRENT_USER\\Software\\Wine\\WineDbg]\n\"ShowCrashDialog\"=dword:00000000\n";
  std::string windows_reg = "Z:" + reg.string();
  std::ranges::replace(windows_reg, '/', '\\');
  // Windows 11 (build 26100) as Creative Cloud expects, and the WinRT notifications AdobeWine adds.
  for (const std::vector<std::string>& args : {std::vector<std::string>{"regedit", "/S", windows_reg},
                                               {"winecfg", "/v", "win11"}, {"regsvr32", "/s", "windows.ui.dll"}}) {
    const auto ran = runner::RunWine(runners, host, args);
    if (!ran) return std::unexpected(ran.error());
    if (ran->exit_code != 0) return Err("install_failed", std::format("wine {} exited {}", args[0], ran->exit_code));
  }

  // vkd3d-proton behind AdobeWine's shim, which retries the core feature level Adobe's AI features ask for at 11_0.
  const auto wine = runner::ResolveWineBinary(runners, host);
  if (!wine) return std::unexpected(wine.error());
  const fs::path shim = wine->parent_path().parent_path() / kExtras / "d3d12.dll";
  const fs::path system32 = fs::path(host.data_dir) / "drive_c" / "windows" / "system32";
  if (fs::is_regular_file(shim, ec) && fs::is_regular_file(system32 / "d3d12.dll", ec) &&
      !fs::exists(system32 / "d3d12_vkd3d.dll", ec)) {
    fs::rename(system32 / "d3d12.dll", system32 / "d3d12_vkd3d.dll", ec);
    if (!ec) fs::copy_file(shim, system32 / "d3d12.dll", ec);
    if (ec) return Err("copy_failed", std::format("couldn't install AdobeWine's Direct3D 12 shim: {}", ec.message()));
  }
  return {};
}

void UseIeSignIn(const fs::path& prefix) {
  const fs::path helper = prefix / "drive_c/Program Files (x86)/Common Files/Adobe/Adobe Desktop Common/NGL/adobe_licensing_wf.exe";
  std::error_code ec;
  if (fs::is_regular_file(helper, ec)) fs::rename(helper, fs::path(helper) += ".disabled", ec);
}

}  // namespace mira::launchers::adobewine
