#include "updates/Updates.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>

#include "core/Log.h"
#include "core/Paths.h"
#include "emulators/Emulators.h"
#include "library/Stores.h"
#include "modmanagers/ModManagers.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "runner/ProtonRunner.h"
#include "runner/RunnerUpdates.h"
#include "runner/Winetricks.h"

#ifndef MIRA_VERSION
#define MIRA_VERSION "0"
#endif

namespace mira::updates {
namespace {
namespace fs = std::filesystem;

std::string WithoutV(const std::string& tag) {
  return tag.starts_with('v') ? tag.substr(1) : tag;
}

// Whether `output` contains `version` as a whole token, not as part of a longer version.
bool MentionsVersion(const std::string& output, const std::string& version) {
  std::string spaced = output;
  std::ranges::replace_if(
      spaced,
      [](unsigned char c) {
        return !std::isalnum(c) && c != '.' && c != '-' && c != '_' && c != '+';
      },
      ' ');
  std::istringstream tokens(spaced);
  for (std::string token; tokens >> token;) {
    if (WithoutV(token) == version) return true;
  }
  return false;
}

// A moving tag (DuckStation's "latest") is compared by when it was published.
bool Behind(const runner::InstalledTool& installed, const runner::ReleaseAsset& latest) {
  if (installed.tag != latest.tag) return true;
  return !installed.published_at.empty() && latest.published_at > installed.published_at;
}

constexpr const char* kMiraRepo = "Mira-Launcher/Mira";

std::string LatestUrl() {
  return std::format("https://api.github.com/repos/{}/releases/latest", kMiraRepo);
}

bool IsNewer(const std::string& tag) {
  return strverscmp(WithoutV(tag).c_str(), MIRA_VERSION) > 0;
}

// This process's own: it doesn't change while running, and asking takes a package manager's time.
const std::string& InstallKind() {
  static const std::string kind = [] {
    const char* appimage = std::getenv("APPIMAGE");
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ClassifyInstall(appimage != nullptr ? appimage : "", exe, system::DetectDistro().family,
                           [](const std::vector<std::string>& argv) {
                             Command command;
                             command.argv = argv;
                             const auto result = runner::RunAndWait(command);
                             return result && result->exit_code == 0;
                           });
  }();
  return kind;
}

// The tag `fetch` finds, kept for 10 minutes: GitHub allows 60 unauthenticated requests an hour.
Result<std::string> CachedTag(const std::string& key, bool refresh,
                              const std::function<Result<std::string>()>& fetch) {
  using Clock = std::chrono::steady_clock;
  static std::mutex mutex;
  static std::map<std::string, std::pair<Clock::time_point, std::string>> cache;
  {
    std::lock_guard lock(mutex);
    const auto found = cache.find(key);
    if (!refresh && found != cache.end() &&
        Clock::now() - found->second.first < std::chrono::minutes(10))
      return found->second.second;
  }
  auto tag = fetch();
  if (tag) {
    std::lock_guard lock(mutex);
    cache[key] = {Clock::now(), *tag};
  }
  return tag;
}

Result<std::string> LatestTag(const std::string& url, const std::string& pattern,
                              const std::vector<std::string>& exclude, bool refresh) {
  return CachedTag(url, refresh, [&]() -> Result<std::string> {
    const auto latest = runner::LatestRelease(url, pattern, exclude);
    if (!latest) return std::unexpected(latest.error());
    return latest->tag;
  });
}

std::optional<Update> MiraUpdate(bool refresh) {
  const auto latest = LatestTag(LatestUrl(), "*.AppImage", {}, refresh);
  if (!latest) {
    log::Warn("couldn't check for a Mira update: {}", latest.error().message);
    return std::nullopt;
  }
  if (!IsNewer(*latest)) return std::nullopt;
  return Update{.kind = "mira",
                .id = "mira",
                .name = "Mira",
                .installed = MIRA_VERSION,
                .latest = *latest,
                .url = std::format("https://github.com/{}/releases/tag/{}", kMiraRepo, *latest),
                .install = InstallKind()};
}

std::optional<Update> ModManagerUpdate(const modmanagers::ModManager& manager, bool refresh) {
  const std::string installed = modmanagers::InstalledVersion(manager);
  if (installed.empty()) return std::nullopt;
  const auto latest = LatestTag(manager.release_url, manager.asset, manager.exclude, refresh);
  if (!latest) {
    log::Warn("couldn't check for a {} update: {}", manager.name, latest.error().message);
    return std::nullopt;
  }
  if (*latest == installed) return std::nullopt;
  return Update{.kind = "mod_manager",
                .id = manager.id,
                .name = manager.name,
                .installed = installed,
                .latest = *latest,
                .url = ""};
}

fs::path PackageDir() {
  return paths::EnvOr("XDG_CACHE_HOME", paths::Home() / ".cache") / "mira" / "update";
}

std::optional<Update> EmulatorUpdate(const config::Config& config,
                                     const emulators::Emulator& emulator) {
  const auto installed = runner::InstalledToolOf(config, emulator.id);
  if (!installed || emulators::Binary(config, emulator) != installed->binary) return std::nullopt;
  const auto latest = emulators::Latest(config, emulator);
  if (!latest) {
    log::Warn("couldn't check for a {} update: {}", emulator.name, latest.error().message);
    return std::nullopt;
  }
  if (!Behind(*installed, *latest)) return std::nullopt;
  return Update{.kind = "emulator",
                .id = emulator.id,
                .name = emulator.name,
                .installed = installed->tag,
                .latest = latest->tag,
                .url = ""};
}

// Whether `path` is a tool Mira downloaded, under its tools folder.
bool Managed(const config::Config& config, const std::string& path) {
  const fs::path relative =
      fs::path(path).lexically_relative(config.File().parent_path() / "tools");
  return !path.empty() && !relative.empty() && *relative.begin() != "..";
}

// A tool Mira downloaded to `path`, from the releases of `release_kind`. One downloaded before Mira
// kept a record of its tag is compared by the version it prints.
std::optional<Update> ToolUpdate(const config::Config& config, const std::string& id,
                                 const std::string& name, const std::string& release_kind,
                                 const std::string& path) {
  if (!Managed(config, path)) return std::nullopt;
  const fs::path relative =
      fs::path(path).lexically_relative(config.File().parent_path() / "tools");
  const auto releases = runner::ListReleases(config, release_kind);
  if (!releases || releases->empty()) {
    if (!releases) log::Warn("couldn't check for a {} update: {}", name, releases.error().message);
    return std::nullopt;
  }
  const runner::ReleaseAsset& latest = releases->front();
  std::string installed;
  if (const auto record = runner::InstalledToolOf(config, relative.begin()->string())) {
    if (!Behind(*record, latest)) return std::nullopt;
    installed = record->tag;
  } else {
    installed = runner::ToolVersion(path);
    if (installed.empty() || MentionsVersion(installed, WithoutV(latest.tag))) return std::nullopt;
  }
  return Update{.kind = "tool",
                .id = id,
                .name = name,
                .installed = installed,
                .latest = latest.tag,
                .url = ""};
}

// Only the copy Mira downloaded, by the version its script names.
std::optional<Update> WinetricksUpdate(const config::Config& config, bool refresh) {
  const std::string path = runner::WinetricksPath();
  if (!Managed(config, path)) return std::nullopt;
  const std::string installed = runner::InstalledWinetricksVersion(path);
  if (installed.empty()) return std::nullopt;
  const auto latest = CachedTag("winetricks", refresh, runner::LatestWinetricksTag);
  if (!latest) {
    log::Warn("couldn't check for a winetricks update: {}", latest.error().message);
    return std::nullopt;
  }
  if (*latest == installed) return std::nullopt;
  return Update{.kind = "tool",
                .id = "winetricks",
                .name = "winetricks",
                .installed = installed,
                .latest = *latest,
                .url = ""};
}

}  // namespace

std::string ClassifyInstall(const std::string& appimage, const fs::path& exe, system::Family family,
                            const std::function<bool(const std::vector<std::string>&)>& succeeds) {
  if (!appimage.empty()) return "appimage";
  const std::string path = exe.string();
  if (!path.starts_with("/usr/") && !path.starts_with("/opt/")) return "none";
  if (family == system::Family::Debian && succeeds({"dpkg", "-S", path})) return "deb";
  if ((family == system::Family::Fedora || family == system::Family::Suse) &&
      succeeds({"rpm", "-qf", path}))
    return "rpm";
  return "manual";
}

Result<nlohmann::json> DownloadMira(
    const std::function<void(int percent, const std::string& message)>& on_progress) {
  const std::string& kind = InstallKind();
  if (kind != "appimage" && kind != "deb" && kind != "rpm")
    return Err("update_unsupported", "this copy of Mira can't update itself");
  const auto latest = runner::LatestRelease(
      LatestUrl(), std::format("*.{}", kind == "appimage" ? "AppImage" : kind));
  if (!latest) return std::unexpected(latest.error());
  if (!IsNewer(latest->tag)) return Err("no_update", "this is already the latest Mira");
  const auto progress = [&on_progress, total = latest->size_bytes](double fraction) {
    constexpr std::int64_t kMegabyte = 1000 * 1000;
    on_progress(
        static_cast<int>(fraction * 100),
        std::format("{} MB of {} MB", static_cast<std::int64_t>(fraction * total) / kMegabyte,
                    total / kMegabyte));
  };

  std::error_code ec;
  if (kind == "appimage") {
    const fs::path appimage = std::getenv("APPIMAGE");
    const fs::path folder = appimage.parent_path();
    // Beside it, since only a rename within its folder can't leave half a file behind.
    if (::access(folder.c_str(), W_OK) != 0)
      return Err("update_not_writable",
                 std::format("Mira can't write to {}. Move its AppImage to a folder you own, or "
                             "replace it by hand.",
                             folder.string()));
    fs::path download = appimage;
    download += ".update";
    if (auto downloaded = runner::DownloadWithDigest(*latest, download, progress); !downloaded)
      return std::unexpected(downloaded.error());
    fs::permissions(download,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    ec);
    if (!ec) fs::rename(download, appimage, ec);
    if (ec) {
      std::error_code ignored;
      fs::remove(download, ignored);
      return Err("update_failed",
                 std::format("couldn't replace {}: {}", appimage.string(), ec.message()));
    }
    log::Info("replaced {} with Mira {}", appimage.string(), latest->tag);
    return nlohmann::json{{"install", nlohmann::json::array()}};
  }

  const fs::path folder = PackageDir();
  fs::create_directories(folder, ec);
  if (ec)
    return Err("update_failed",
               std::format("couldn't create {}: {}", folder.string(), ec.message()));
  const fs::path package = folder / latest->asset_name;
  if (auto downloaded = runner::DownloadWithDigest(*latest, package, progress); !downloaded)
    return std::unexpected(downloaded.error());
  const auto install = system::InstallFileCommand(system::DetectDistro().family, package.string());
  if (install.empty()) {
    RemoveDownloadedMira();
    return Err("update_unsupported", "Mira doesn't know how to install a package on this system");
  }
  return nlohmann::json{{"install", install}};
}

void RemoveDownloadedMira() {
  std::error_code ec;
  fs::remove_all(PackageDir(), ec);
}

std::vector<Update> FindUpdates(config::Config& config, bool refresh) {
  if (refresh) runner::ForgetCachedReleases();
  std::vector<std::future<std::optional<Update>>> checks;
  checks.push_back(std::async(std::launch::async, MiraUpdate, refresh));
  for (const emulators::Emulator& emulator : emulators::All()) {
    if (!emulators::Downloads(emulator)) continue;
    checks.push_back(std::async(std::launch::async,
                                [&config, &emulator] { return EmulatorUpdate(config, emulator); }));
  }
  for (const library::Store& store : library::AllStores()) {
    checks.push_back(std::async(std::launch::async, [&config, &store] {
      return ToolUpdate(config, store.id, store.tool, store.release_kind,
                        store.status(config).tool.path);
    }));
    // The store's setup also updates its helper. One the tool fetches itself has no release to
    // compare with.
    if (store.helper_status && store.helper_release_kind) {
      checks.push_back(std::async(std::launch::async, [&config, &store]() -> std::optional<Update> {
        const std::string path = store.helper_status(config).path;
        if (!path.empty())
          return ToolUpdate(config, store.id, store.helper, store.helper_release_kind, path);
        // A store set up before it had a helper: offered with nothing installed.
        if (!Managed(config, store.status(config).tool.path)) return std::nullopt;
        const auto releases = runner::ListReleases(config, store.helper_release_kind);
        if (!releases || releases->empty()) return std::nullopt;
        return Update{.kind = "tool",
                      .id = store.id,
                      .name = store.helper,
                      .installed = "",
                      .latest = releases->front().tag,
                      .url = ""};
      }));
    }
  }
  checks.push_back(std::async(std::launch::async, [&config] {
    return ToolUpdate(config, "umu", "umu", "umu", runner::UmuRunPath());
  }));
  checks.push_back(std::async(std::launch::async, WinetricksUpdate, std::cref(config), refresh));
  for (const modmanagers::ModManager& manager : modmanagers::All()) {
    checks.push_back(std::async(std::launch::async, ModManagerUpdate, std::cref(manager), refresh));
  }

  std::vector<Update> updates;
  for (auto& check : checks) {
    if (auto update = check.get()) updates.push_back(std::move(*update));
  }
  const runner::RunnerRegistry registry(config);
  for (const runner::RunnerUpdate& update : runner::FindRunnerUpdates(config, registry)) {
    updates.push_back(
        {.kind = "runner",
         .id = update.build.Reference(),
         .name = update.family.label,
         .installed = runner::BuildLabel(update.build.kind, update.build.name),
         .latest = runner::BuildLabel(update.build.kind,
                                      runner::ReleaseName(update.build.kind, update.latest)),
         .url = "",
         .release = runner::ReleaseName(update.build.kind, update.latest)});
  }
  return updates;
}

}  // namespace mira::updates
