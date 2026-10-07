#include "runner/Downloader.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include <json.hpp>

#include "config/RunnerSources.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "runner/Curl.h"
#include "runner/Exec.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

struct Source {
  std::string repo;
  std::string asset_pattern;
  std::vector<std::string> exclude;
};

Result<Source> SourceFor(const config::Config& config, const std::string& kind) {
  if (kind == "proton" || kind == "wine") {
    const RunnerFamily family = Families(config, kind).front();
    return Source{.repo = family.repo, .asset_pattern = family.asset_pattern, .exclude = family.exclude};
  }
  // Not a runner kind: the store CLIs (legendary, gogdl, butler, nile,
  // humble-cli) share the same "list a GitHub repo's releases, filter assets
  // by glob" shape, so they reuse ListReleases rather than duplicating the
  // GitHub API call. Each one's repo and pattern are settings.
  if (kind == "legendary" || kind == "gog" || kind == "itch" || kind == "amazon" || kind == "humble") {
    const std::string prefix = "runner_sources." + kind;
    return Source{.repo = config.GetString(prefix + ".repo"),
                  .asset_pattern = config.GetString(prefix + ".asset_pattern"),
                  .exclude = {}};
  }
  if (kind == "umu") {
    return Source{.repo = std::string(config::runner_sources::kUmuLauncherRepo),
                  .asset_pattern = std::string(config::runner_sources::kUmuLauncherAssetPattern), .exclude = {}};
  }
  return Err("unknown_runner_kind", std::format("no downloadable source for kind \"{}\"", kind));
}

std::filesystem::path InstallDirFor(const config::Config& config, const std::string& kind) {
  const auto paths = config.GetPathArray(kind == "proton" ? "runner_search_paths" : "wine_search_paths");
  return paths.empty() ? fs::path() : paths.front();
}

std::string ArchiveStem(const std::string& asset_name) {
  for (const char* ext : {".tar.gz", ".tar.xz", ".tgz", ".zip"}) {
    if (asset_name.ends_with(ext)) return asset_name.substr(0, asset_name.size() - std::strlen(ext));
  }
  return asset_name;
}

// A release's checksum for `tarball_name`: "<stem>.sha512sum" or
// "<stem>.sha256sum" beside it, or one sha256sums.txt for the whole release
// (Kron4ek).
void FindChecksum(const json& assets, const std::string& tarball_name, ReleaseAsset& out) {
  const std::string stem = ArchiveStem(tarball_name);
  std::string list;
  for (const auto& asset : assets) {
    const std::string name = asset.value("name", std::string());
    const std::string url = asset.value("browser_download_url", std::string());
    if (name == stem + ".sha512sum") {
      out.checksum_url = url;
      out.checksum_is_list = false;
      out.checksum_is_sha256 = false;
      return;
    }
    if (name == stem + ".sha256sum") {
      out.checksum_url = url;
      out.checksum_is_sha256 = true;
    }
    if (name == "sha256sums.txt") list = url;
  }
  if (!out.checksum_url.empty() || list.empty()) return;
  out.checksum_url = list;
  out.checksum_is_list = true;
  out.checksum_is_sha256 = true;
}

bool Excluded(const std::vector<std::string>& exclude, const std::string& text) {
  return std::ranges::any_of(exclude, [&](const std::string& word) { return text.contains(word); });
}

// Lists `repo`'s releases, keeping the first asset per release that matches.
Result<std::vector<ReleaseAsset>> FetchReleases(const std::string& repo, const std::string& pattern,
                                                const std::vector<std::string>& exclude) {
  const Result<json> listed = CurlJson(std::format("https://api.github.com/repos/{}/releases?per_page=10", repo));
  if (!listed) return Err("github_api_error", std::format("couldn't list releases for {}: {}", repo, listed.error().message),
                          kConnectionHint);
  const json& parsed = *listed;
  if (!parsed.is_array()) {
    return Err("github_api_error", std::format("couldn't list releases for {}: {}", repo, parsed.dump()),
               kConnectionHint);
  }

  std::vector<ReleaseAsset> releases;
  for (const auto& release : parsed) {
    if (!release.is_object() || !release.contains("assets") || !release["assets"].is_array()) continue;
    const json& assets = release["assets"];
    for (const auto& asset : assets) {
      const std::string name = core::JsonString(asset, "name");
      if (!strings::GlobMatch(pattern, name) || Excluded(exclude, name)) continue;

      ReleaseAsset entry;
      entry.tag = core::JsonString(release, "tag_name");
      entry.asset_name = name;
      entry.download_url = core::JsonString(asset, "browser_download_url");
      entry.size_bytes = core::JsonInt(asset, "size");
      entry.published_at = core::JsonString(release, "published_at");
      FindChecksum(assets, name, entry);
      releases.push_back(std::move(entry));
      break;  // one matching asset per release is expected
    }
  }
  return releases;
}

// Downloads `asset` to `target` and verifies it against
// asset.checksum_url if the release had one -- the part DownloadAndInstall
// and InstallToolBinary share before they diverge on what to do with the
// downloaded file (extract a tarball vs. chmod a bare/extracted binary).
// `target`'s own filename must equal asset.asset_name for the checksum
// file's recorded name to match what sha512sum -c finds on disk --
// callers that want a different final name rename after this returns.
Result<void> DownloadVerified(const ReleaseAsset& asset, const fs::path& target) {
  std::error_code ec;
  if (auto downloaded = CurlDownload(asset.download_url, target); !downloaded) return downloaded;

  if (asset.checksum_url.empty()) {
    log::Warn("no checksum available for {}, installing unverified", asset.asset_name);
    return {};
  }

  const fs::path checksum_file =
      target.parent_path() / (asset.asset_name + (asset.checksum_is_sha256 ? ".sha256sum" : ".sha512sum"));
  if (auto fetched = CurlDownload(asset.checksum_url, checksum_file); !fetched) {
    fs::remove(target, ec);
    fs::remove(checksum_file, ec);
    return Err("checksum_fetch_failed", "couldn't fetch the checksum file to verify the download");
  }

  if (asset.checksum_is_sha256) {
    // Keep only this asset's line, so other files a list names aren't checked.
    std::ifstream in(checksum_file);
    std::string line, mine;
    while (std::getline(in, line)) {
      if (line.ends_with(" " + asset.asset_name) || line.ends_with("*" + asset.asset_name)) mine = line;
    }
    in.close();
    if (mine.empty()) {
      fs::remove(target, ec);
      fs::remove(checksum_file, ec);
      return Err("checksum_missing", std::format("the checksum file doesn't list {}", asset.asset_name));
    }
    std::ofstream(checksum_file, std::ios::trunc) << mine << "\n";
  }

  Command verify;
  verify.argv = {asset.checksum_is_sha256 ? "sha256sum" : "sha512sum", "-c", checksum_file.filename().string()};
  verify.cwd = target.parent_path();
  const Result<ExecResult> verified = RunAndWait(verify);
  fs::remove(checksum_file, ec);
  if (!verified || verified->exit_code != 0) {
    fs::remove(target, ec);
    return Err("checksum_mismatch",
              "downloaded file's checksum didn't match, so it was discarded instead of installing a "
              "corrupted or tampered build");
  }
  return {};
}

// Recursively searches `dir` for a regular file named `name`, for pulling
// a known binary out of an archive whose internal layout isn't assumed
// (e.g. butler-linux-amd64.zip nests everything under "linux-amd64/").
std::optional<fs::path> FindFileNamed(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  std::error_code entry_ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(entry_ec) && it->path().filename() == name) return it->path();
  }
  return std::nullopt;
}

// Moves everything in `from` into `to`, all or nothing: an entry already in `to` fails it before
// anything moves, and a move that fails takes the ones already moved back out.
Result<void> MoveAllInto(const fs::path& from, const fs::path& to) {
  const std::vector<fs::path> entries = paths::ListDir(from);
  std::error_code ec;
  for (const fs::path& entry : entries) {
    if (fs::exists(to / entry.filename(), ec)) {
      return Err("install_failed",
                 std::format("{} is already there", (to / entry.filename()).string()));
    }
  }
  std::vector<fs::path> moved;
  for (const fs::path& entry : entries) {
    fs::rename(entry, to / entry.filename(), ec);
    if (ec) {
      const std::string message = ec.message();
      for (const fs::path& done : moved) fs::remove_all(done, ec);
      return Err("install_failed", std::format("couldn't move {} into place: {}",
                                               entry.filename().string(), message));
    }
    moved.push_back(to / entry.filename());
  }
  return {};
}

bool IsZip(const std::string& asset_name) { return asset_name.ends_with(".zip"); }
bool IsTarball(const std::string& asset_name) {
  return asset_name.ends_with(".tar") || asset_name.ends_with(".tar.gz") || asset_name.ends_with(".tar.xz") ||
         asset_name.ends_with(".tgz");
}

Result<void> Chmod(const fs::path& path) {
  std::error_code ec;
  fs::permissions(path,
                  fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                      fs::perms::others_exec,
                  ec);
  if (ec) return Err("chmod_failed", ec.message());
  return {};
}

}  // namespace

Result<std::vector<ReleaseAsset>> ListReleases(const config::Config& config, const std::string& kind) {
  if (kind == "proton" || kind == "wine") return ListFamilyReleases(Families(config, kind).front());
  const Result<Source> source = SourceFor(config, kind);
  if (!source) return std::unexpected(source.error());
  return FetchReleases(source->repo, source->asset_pattern, source->exclude);
}

std::vector<RunnerFamily> Families(const config::Config& config, const std::string& kind) {
  namespace rs = config::runner_sources;
  std::vector<RunnerFamily> all = {
      {.id = "proton_ge", .kind = "proton", .label = "GE-Proton",
       .repo = config.GetString("runner_sources.proton_ge.repo"),
       .asset_pattern = config.GetString("runner_sources.proton_ge.asset_pattern"),
       .build_pattern = "GE-Proton*", .exclude = {}},
      {.id = "proton_cachyos", .kind = "proton", .label = "Proton-CachyOS",
       .repo = std::string(rs::kProtonCachyOSRepo), .asset_pattern = std::string(rs::kProtonCachyOSAssetPattern),
       .build_pattern = "cachyos-*-slr", .exclude = {}},
      {.id = "proton_umu", .kind = "proton", .label = "UMU-Proton",
       .repo = std::string(rs::kUmuProtonRepo), .asset_pattern = "UMU-Proton-*.tar.gz",
       .build_pattern = "UMU-Proton-*", .exclude = {}},
      {.id = "proton_em", .kind = "proton", .label = "Proton-EM",
       .repo = std::string(rs::kProtonEMRepo), .asset_pattern = "proton-EM-*.tar.xz",
       .build_pattern = "proton-EM-*", .exclude = {}},
      {.id = "proton_sarek", .kind = "proton", .label = "Proton-Sarek (older GPUs)",
       .repo = std::string(rs::kProtonSarekRepo), .asset_pattern = "Proton-Sarek*.tar.gz",
       .build_pattern = "Proton-Sarek*", .exclude = {"async"}},
      {.id = "wine_staging_tkg", .kind = "wine", .label = "Wine staging-tkg (Kron4ek)",
       .repo = std::string(rs::kKron4ekRepo), .asset_pattern = "wine-*-staging-tkg-amd64.tar.xz",
       .build_pattern = "wine-*-staging-tkg-amd64", .exclude = {}},
      {.id = "wine_staging", .kind = "wine", .label = "Wine staging (Kron4ek)",
       .repo = std::string(rs::kKron4ekRepo), .asset_pattern = "wine-*-staging-amd64.tar.xz",
       .build_pattern = "wine-*-staging-amd64", .exclude = {"tkg"}},
      {.id = "wine_vanilla", .kind = "wine", .label = "Wine (Kron4ek)",
       .repo = std::string(rs::kKron4ekRepo), .asset_pattern = "wine-*-amd64.tar.xz",
       .build_pattern = "wine-*-amd64", .exclude = {"staging"}},
      {.id = "wine_ge", .kind = "wine", .label = "Wine-GE (archived)",
       .repo = config.GetString("runner_sources.wine_ge.repo"),
       .asset_pattern = config.GetString("runner_sources.wine_ge.asset_pattern"),
       .build_pattern = "lutris-GE-Proton*", .exclude = {}},
      {.id = "wine_lutris", .kind = "wine", .label = "Lutris Wine (archived)",
       .repo = std::string(rs::kLutrisWineRepo), .asset_pattern = "wine-lutris-*-x86_64.tar.xz",
       .build_pattern = "lutris-*-x86_64", .exclude = {"GE-Proton"}},
  };
  if (kind.empty()) return all;
  std::erase_if(all, [&](const RunnerFamily& family) { return family.kind != kind; });
  return all;
}

std::optional<RunnerFamily> FindFamily(const config::Config& config, const std::string& id) {
  for (RunnerFamily& family : Families(config, "")) {
    if (family.id == id) return std::move(family);
  }
  return std::nullopt;
}

std::optional<RunnerFamily> FamilyOfBuild(const config::Config& config, const std::string& kind,
                                          const std::string& name, const std::string& folder) {
  const auto matches = [](const RunnerFamily& family, const std::string& text) {
    return strings::GlobMatch(family.build_pattern, text) && !Excluded(family.exclude, text);
  };
  for (RunnerFamily& family : Families(config, kind)) {
    if (matches(family, folder) || (kind == "proton" && matches(family, name))) return std::move(family);
  }
  return std::nullopt;
}

bool IsInstalledAs(const std::string& kind, const std::string& name, const std::string& folder,
                   const ReleaseAsset& asset) {
  const std::string stem = ArchiveStem(asset.asset_name);
  // Wine-GE's "wine-lutris-GE-…" unpacks to "lutris-GE-…"; Proton builds
  // name themselves after the release tag in their version file.
  return folder == stem || "wine-" + folder == stem || (kind == "proton" && name == asset.tag);
}

std::string ReleaseName(const std::string& kind, const ReleaseAsset& asset) {
  return kind == "wine" ? ArchiveStem(asset.asset_name) : asset.tag;
}

std::string BuildLabel(const std::string& kind, const std::string& name) {
  if (kind != "wine") return name;
  std::string text = name.starts_with("wine-lutris-") ? name.substr(5) : name;
  for (const char* suffix : {"-x86_64", "-amd64"}) {
    if (text.ends_with(suffix)) text.resize(text.size() - std::strlen(suffix));
  }
  if (text.starts_with("lutris-GE-Proton")) return "Wine-GE " + text.substr(16);
  if (text.starts_with("lutris-")) return "Lutris Wine " + text.substr(7);
  if (text.starts_with("wine-")) {
    // "wine-11.18-staging-tkg" reads "Wine 11.18 staging-tkg".
    std::string rest = text.substr(5);
    if (const auto dash = rest.find('-'); dash != std::string::npos) rest[dash] = ' ';
    return "Wine " + rest;
  }
  return name;
}

Result<std::vector<ReleaseAsset>> ListFamilyReleases(const RunnerFamily& family) {
  using Clock = std::chrono::steady_clock;
  struct Cached {
    Clock::time_point at;
    std::vector<ReleaseAsset> releases;
  };
  static std::mutex mutex;
  static std::map<std::string, Cached> cache;
  const std::string key = family.repo + "|" + family.asset_pattern;
  {
    std::lock_guard lock(mutex);
    if (auto it = cache.find(key); it != cache.end() && Clock::now() - it->second.at < std::chrono::minutes(10)) {
      return it->second.releases;
    }
  }
  auto releases = FetchReleases(family.repo, family.asset_pattern, family.exclude);
  if (!releases) return releases;
  std::lock_guard lock(mutex);
  cache[key] = {Clock::now(), *releases};
  return releases;
}

Result<void> DownloadAndInstall(const config::Config& config, const std::string& kind,
                                const ReleaseAsset& asset, const std::function<void(double)>& on_progress) {
  // Two requests for one build would download and extract into the same folder.
  static std::mutex in_flight_mutex;
  static std::set<std::string> in_flight;
  const std::string in_flight_key = kind + "|" + asset.tag;
  {
    const std::lock_guard lock(in_flight_mutex);
    if (!in_flight.insert(in_flight_key).second) {
      return Err("already_installing", std::format("{} {} is already being installed", kind, asset.tag));
    }
  }
  const struct Release {
    const std::string& key;
    ~Release() {
      const std::lock_guard lock(in_flight_mutex);
      in_flight.erase(key);
    }
  } release{in_flight_key};

  const fs::path install_dir = InstallDirFor(config, kind);
  if (install_dir.empty()) {
    return Err("no_search_path", std::format("no {} search path configured to install into", kind));
  }
  std::error_code ec;
  fs::create_directories(install_dir, ec);
  if (ec) return Err("install_dir_failed", ec.message());

  // Downloaded into the install dir itself so a same-filesystem rename
  // isn't a concern and there's nothing to clean up across filesystems;
  // both the archive and (if present) its checksum file are removed again
  // once extraction succeeds.
  const fs::path archive = install_dir / asset.asset_name;
  std::jthread watch;
  if (on_progress && asset.size_bytes > 0) {
    // curl writes straight into `archive`, so its size against the release's says how far along it is.
    watch = std::jthread([&archive, &on_progress, total = asset.size_bytes](std::stop_token stop) {
      int reported = -1;
      while (!stop.stop_requested()) {
        std::error_code size_ec;
        const auto size = fs::file_size(archive, size_ec);
        const int percent = size_ec ? 0 : static_cast<int>(std::min<std::uintmax_t>(size * 100 / total, 100));
        if (percent != reported) on_progress((reported = percent) / 100.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    });
  }
  auto downloaded = DownloadVerified(asset, archive);
  if (watch.joinable()) {
    watch.request_stop();
    watch.join();
  }
  if (!downloaded) return downloaded;

  // Extracted aside and moved in, so a failed extraction never leaves a half-built runner Discover accepts.
  const fs::path staging = install_dir / (".extracting-" + asset.asset_name);
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  Result<void> extracted = Extract(archive, staging);
  fs::remove(archive, ec);
  if (extracted) extracted = MoveAllInto(staging, install_dir);
  fs::remove_all(staging, ec);
  return extracted;
}

Result<fs::path> InstallToolBinary(const config::Config& config, const std::string& tool_name,
                                   const ReleaseAsset& asset, const std::string& binary_name) {
  const fs::path tool_dir = config.File().parent_path() / "tools" / tool_name;
  std::error_code ec;
  fs::create_directories(tool_dir, ec);
  if (ec) return Err("install_dir_failed", ec.message());

  const fs::path downloaded = tool_dir / asset.asset_name;
  if (auto result = DownloadVerified(asset, downloaded); !result) return std::unexpected(result.error());

  const fs::path target = tool_dir / binary_name;
  if (IsZip(asset.asset_name) || IsTarball(asset.asset_name)) {
    // Searched in its own extraction, so a binary left from an older release's layout is never
    // found instead. Only the archive's own entries are replaced: the folder also holds tool data
    // (itch's butler.db).
    const fs::path staging = tool_dir / (".extracting-" + asset.asset_name);
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    const Result<void> extracted = Extract(downloaded, staging);
    fs::remove(downloaded, ec);
    const auto found = extracted ? FindFileNamed(staging, binary_name) : std::nullopt;
    if (!found) {
      fs::remove_all(staging, ec);
      if (!extracted) return std::unexpected(extracted.error());
      return Err("binary_not_found",
                std::format("{} didn't contain a file named \"{}\"", asset.asset_name, binary_name));
    }
    for (const fs::path& entry : paths::ListDir(staging))
      fs::remove_all(tool_dir / entry.filename(), ec);
    const Result<void> moved = MoveAllInto(staging, tool_dir);
    fs::remove_all(staging, ec);
    if (!moved) return std::unexpected(moved.error());
    const fs::path binary = tool_dir / found->lexically_relative(staging);
    if (auto chmodded = Chmod(binary); !chmodded) return std::unexpected(chmodded.error());
    return binary;
  }

  fs::rename(downloaded, target, ec);
  if (ec) {
    fs::remove(downloaded, ec);
    return Err("install_failed", ec.message());
  }
  if (auto chmodded = Chmod(target); !chmodded) return std::unexpected(chmodded.error());
  return target;
}

}  // namespace mira::runner
