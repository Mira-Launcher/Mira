#pragma once

// Compiled-in defaults for where Mira downloads runner builds from, the same
// treatment as KnownExePatterns.h: plain data, not user-editable settings
// (those are the runner_sources.*.repo / .*.asset_pattern schema keys this
// feeds as defaults, in case the URL itself needs to change without a
// rebuild; see Schema.cpp).

#include <string_view>

namespace mira::config::runner_sources {

// GitHub "owner/repo" to list releases of, and the glob a release's asset
// list is filtered to (excludes AArch64 builds, checksum files, etc.). Proton-GE
// in particular ships both x86_64 and aarch64 tarballs per release).
inline constexpr std::string_view kProtonGERepo = "GloriousEggroll/proton-ge-custom";
inline constexpr std::string_view kProtonGEAssetPattern = "*x86_64.tar.gz";

inline constexpr std::string_view kWineGERepo = "GloriousEggroll/wine-ge-custom";
inline constexpr std::string_view kWineGEAssetPattern = "*x86_64.tar.xz";

// CachyOS's Proton, the generic x86_64 build (not _v3, not arm64).
inline constexpr std::string_view kProtonCachyOSRepo = "CachyOS/proton-cachyos";
inline constexpr std::string_view kProtonCachyOSAssetPattern = "proton-cachyos-*-slr-x86_64.tar.xz";

inline constexpr std::string_view kUmuProtonRepo = "Open-Wine-Components/umu-proton";
inline constexpr std::string_view kProtonEMRepo = "Etaash-mathamsetty/Proton";
inline constexpr std::string_view kProtonSarekRepo = "pythonlover02/Proton-Sarek";
inline constexpr std::string_view kLutrisWineRepo = "lutris/wine";

// umu-launcher, which every Proton build runs through. The zipapp needs
// only python3; it unpacks to umu/umu-run.
inline constexpr std::string_view kUmuLauncherRepo = "Open-Wine-Components/umu-launcher";
inline constexpr std::string_view kUmuLauncherAssetPattern = "umu-launcher-*-zipapp.tar";

// Kron4ek's Wine builds: vanilla, staging and staging-tkg in every release,
// checksummed by one sha256sums.txt.
inline constexpr std::string_view kKron4ekRepo = "Kron4ek/Wine-Builds";

// Legendary (a native Epic Games Store CLI client) publishes a single file per
// architecture, not an archive; see runner::InstallLegendaryBinary
// (src/epic/Legendary.h) rather than runner::DownloadAndInstall, which assumes
// a tarball. Releases up to 0.20.34 named it plain "legendary"; achievements
// need 0.21.1, so the old name isn't matched.
inline constexpr std::string_view kLegendaryRepo = "legendary-gl/legendary";
#if defined(__aarch64__)
inline constexpr std::string_view kLegendaryAssetPattern = "legendary_linux_arm64";
#else
inline constexpr std::string_view kLegendaryAssetPattern = "legendary_linux_x64";
#endif

// gogdl (Heroic's GOG downloader) also publishes a standalone Linux
// binary per release, same shape as Legendary: "gogdl_linux_x86_64", no
// archive.
inline constexpr std::string_view kGogdlRepo = "Heroic-Games-Launcher/heroic-gogdl";
inline constexpr std::string_view kGogdlAssetPattern = "gogdl_linux_x86_64";

// comet stands in for GOG Galaxy's communication service so GOG games can unlock achievements.
// A bare binary per architecture.
inline constexpr std::string_view kCometRepo = "imLinguin/comet";
#if defined(__aarch64__)
inline constexpr std::string_view kCometAssetPattern = "comet-aarch64-unknown-linux-gnu";
#else
inline constexpr std::string_view kCometAssetPattern = "comet-x86_64-unknown-linux-gnu";
#endif

// butler (itch.io's own launcher-integration daemon) ships zipped, with
// two shared libraries (7z.so/libc7zip.so) alongside the binary it needs
// at runtime. See
// runner::InstallToolBinary, which extracts and searches rather than
// assuming a bare binary the way Legendary/gogdl's asset is.
inline constexpr std::string_view kButlerRepo = "itchio/butler";
inline constexpr std::string_view kButlerAssetPattern = "butler-linux-amd64.zip";

// humble-cli (unofficial Humble Bundle CLI) ships tarred, with the binary
// itself named for its own platform ("humble-cli-linux-amd64", not just
// "humble-cli").
inline constexpr std::string_view kHumbleCliRepo = "smbl64/humble-cli";
inline constexpr std::string_view kHumbleCliAssetPattern = "humble-cli-linux-amd64.tar.gz";
inline constexpr std::string_view kHumbleCliBinaryName = "humble-cli-linux-amd64";

// nile (Heroic's Amazon Games client) ships one standalone binary per
// release, same shape as gogdl.
inline constexpr std::string_view kNileRepo = "imLinguin/nile";
inline constexpr std::string_view kNileAssetPattern = "nile_linux_x86_64";

}  // namespace mira::config::runner_sources
