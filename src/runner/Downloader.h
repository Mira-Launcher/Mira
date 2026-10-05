#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"

namespace mira::runner {

// One release asset offered for download, already filtered to the single
// asset per release that matches the source's asset_pattern (see
// RunnerSources.h / the runner_sources.* settings), so a caller never has
// to know GitHub's release/asset JSON shape.
struct ReleaseAsset {
  std::string tag;
  std::string asset_name;
  std::string download_url;
  std::string checksum_url;  // empty if the release had no matching .sha512sum asset
  bool checksum_is_sha256 = false;  // else sha512
  bool checksum_is_list = false;    // checksum_url is a sha256sums.txt covering every asset
  std::int64_t size_bytes = 0;
  std::string published_at;
};

// One place runner builds of a kind come from (GE-Proton, Kron4ek staging,
// ...). `build_pattern` recognises an installed build of this family by its
// folder, or for Proton also by its name.
struct RunnerFamily {
  std::string id;
  std::string kind;  // "proton" | "wine"
  std::string label;
  std::string repo;
  std::string asset_pattern;
  std::string build_pattern;
  std::vector<std::string> exclude;  // substrings that rule an asset or build out
};

// Every family for `kind`, preferred first.
std::vector<RunnerFamily> Families(const config::Config& config, const std::string& kind);
std::optional<RunnerFamily> FindFamily(const config::Config& config, const std::string& id);

// The family an installed build belongs to, if any. `folder` is its directory's name.
std::optional<RunnerFamily> FamilyOfBuild(const config::Config& config, const std::string& kind,
                                          const std::string& name, const std::string& folder);

// Whether an installed build (name, folder) is the one `asset` unpacks to.
bool IsInstalledAs(const std::string& kind, const std::string& name, const std::string& folder,
                   const ReleaseAsset& asset);

// What a release is called in lists and download events: the tag for
// Proton, the archive's name for Wine (Kron4ek's tags are bare versions
// shared by every variant).
std::string ReleaseName(const std::string& kind, const ReleaseAsset& asset);

// A readable name for a build or release name: "wine-11.18-staging-tkg-amd64"
// reads "Wine 11.18 staging-tkg", Wine-GE's "lutris-GE-Proton8-26-x86_64"
// reads "Wine-GE 8-26". Proton names pass through.
std::string BuildLabel(const std::string& kind, const std::string& name);

// A family's releases, newest first. Cached for a few minutes, since GitHub
// allows 60 unauthenticated requests an hour.
Result<std::vector<ReleaseAsset>> ListFamilyReleases(const RunnerFamily& family);

// Lists releases of `kind` ("proton", "wine", or a tool such as "legendary",
// "gog" or "umu") from its GitHub source, newest first. Shells out to curl for the GitHub API request rather than linking
// libcurl, consistent with how umu-run/wine are already run as
// subprocesses, and this is occasional, human-triggered traffic, not a hot
// path.
Result<std::vector<ReleaseAsset>> ListReleases(const config::Config& config, const std::string& kind);

// Downloads `asset` (verifying its checksum first, if it has one) and
// extracts it into the right search path for `kind`:
// runner_search_paths[0] for "proton", wine_search_paths[0] for "wine",
// so the next Discover() call finds it. Shells out
// to curl + tar rather than linking an archive/TLS library, for the same
// reason as ListReleases. `on_progress` gets the downloaded fraction (0..1)
// as it changes, when the release's size is known.
Result<void> DownloadAndInstall(const config::Config& config, const std::string& kind,
                                const ReleaseAsset& asset, const std::function<void(double)>& on_progress = {});

// Downloads and installs a tool binary that isn't a runner build (gogdl,
// butler, ...) into "<config dir>/tools/<tool_name>/<binary_name>",
// returning that path. How `asset` unpacks depends on its own name: a
// ".zip"/".tar.*" asset is extracted into that directory first and
// `binary_name` is searched for inside it (some tools ship shared
// libraries the binary needs to find alongside itself at runtime --
// butler's 7z.so/libc7zip.so, for one -- so the archive's own internal
// layout is never assumed, just searched); anything else is installed as
// the bare file directly, renamed to `binary_name`. Verifies
// asset.checksum_url first if the release has one, same as
// DownloadAndInstall.
Result<std::filesystem::path> InstallToolBinary(const config::Config& config, const std::string& tool_name,
                                               const ReleaseAsset& asset, const std::string& binary_name);

}  // namespace mira::runner
