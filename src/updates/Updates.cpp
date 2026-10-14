#include "updates/Updates.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <future>
#include <optional>
#include <sstream>

#include "core/Log.h"
#include "emulators/Emulators.h"
#include "library/Stores.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "runner/ProtonRunner.h"
#include "runner/RunnerUpdates.h"

#ifndef MIRA_VERSION
#define MIRA_VERSION "0"
#endif

namespace mira::updates {
namespace {
namespace fs = std::filesystem;

std::string WithoutV(const std::string& tag) { return tag.starts_with('v') ? tag.substr(1) : tag; }

// Whether `output` contains `version` as a whole token, not as part of a longer version.
bool MentionsVersion(const std::string& output, const std::string& version) {
  std::string spaced = output;
  std::ranges::replace_if(
      spaced, [](unsigned char c) { return !std::isalnum(c) && c != '.' && c != '-' && c != '_' && c != '+'; }, ' ');
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

std::optional<Update> MiraUpdate() {
  const auto latest = runner::LatestRelease(std::format("https://api.github.com/repos/{}/releases/latest", kMiraRepo),
                                            "*.AppImage");
  if (!latest) {
    log::Warn("couldn't check for a Mira update: {}", latest.error().message);
    return std::nullopt;
  }
  if (strverscmp(WithoutV(latest->tag).c_str(), MIRA_VERSION) <= 0) return std::nullopt;
  return Update{.kind = "mira", .id = "mira", .name = "Mira", .installed = MIRA_VERSION, .latest = latest->tag,
                .url = std::format("https://github.com/{}/releases/tag/{}", kMiraRepo, latest->tag)};
}

std::optional<Update> EmulatorUpdate(const config::Config& config, const emulators::Emulator& emulator) {
  const auto installed = runner::InstalledToolOf(config, emulator.id);
  if (!installed || emulators::Binary(config, emulator) != installed->binary) return std::nullopt;
  const auto latest = emulators::Latest(config, emulator);
  if (!latest) {
    log::Warn("couldn't check for a {} update: {}", emulator.name, latest.error().message);
    return std::nullopt;
  }
  if (!Behind(*installed, *latest)) return std::nullopt;
  return Update{.kind = "emulator", .id = emulator.id, .name = emulator.name, .installed = installed->tag,
                .latest = latest->tag, .url = ""};
}

// A tool Mira downloaded to `path`, from the releases of `release_kind`. One downloaded before Mira kept a
// record of its tag is compared by the version it prints.
std::optional<Update> ToolUpdate(const config::Config& config, const std::string& id, const std::string& name,
                                 const std::string& release_kind, const std::string& path) {
  const fs::path tools = config.File().parent_path() / "tools";
  const fs::path relative = fs::path(path).lexically_relative(tools);
  if (path.empty() || relative.empty() || *relative.begin() == "..") return std::nullopt;
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
  return Update{.kind = "tool", .id = id, .name = name, .installed = installed, .latest = latest.tag, .url = ""};
}

}  // namespace

std::vector<Update> FindUpdates(config::Config& config) {
  std::vector<std::future<std::optional<Update>>> checks;
  checks.push_back(std::async(std::launch::async, MiraUpdate));
  for (const emulators::Emulator& emulator : emulators::All()) {
    if (!emulators::Downloads(emulator)) continue;
    checks.push_back(std::async(std::launch::async, [&config, &emulator] { return EmulatorUpdate(config, emulator); }));
  }
  for (const library::Store& store : library::AllStores()) {
    checks.push_back(std::async(std::launch::async, [&config, &store] {
      return ToolUpdate(config, store.id, store.tool, store.release_kind, store.status(config).tool.path);
    }));
  }
  checks.push_back(std::async(std::launch::async,
                              [&config] { return ToolUpdate(config, "umu", "umu", "umu", runner::UmuRunPath()); }));

  std::vector<Update> updates;
  for (auto& check : checks) {
    if (auto update = check.get()) updates.push_back(std::move(*update));
  }
  const runner::RunnerRegistry registry(config);
  for (const runner::RunnerUpdate& update : runner::FindRunnerUpdates(config, registry)) {
    updates.push_back({.kind = "runner", .id = update.build.Reference(), .name = update.family.label,
                       .installed = runner::BuildLabel(update.build.kind, update.build.name),
                       .latest = runner::BuildLabel(update.build.kind,
                                                    runner::ReleaseName(update.build.kind, update.latest)),
                       .url = ""});
  }
  return updates;
}

}  // namespace mira::updates
