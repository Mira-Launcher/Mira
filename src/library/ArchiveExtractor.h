#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.h"

namespace mira::library {

// Includes the first volume of a split archive (Game.part1.rar, Game.7z.001).
bool LooksLikeArchive(const std::filesystem::path& path);

// Game.part2.rar, Game.r00, Game.7z.002, ...: parts that are extracted
// through their first volume, never on their own.
bool IsLaterVolume(const std::filesystem::path& path);

// The first volume a later volume belongs to, if it's there.
std::optional<std::filesystem::path> FirstVolumeOf(const std::filesystem::path& later);

// Every volume of `archive` on disk, `archive` itself first.
std::vector<std::filesystem::path> VolumesOf(const std::filesystem::path& archive);

// True while any process has one of `paths` open for writing.
bool AnyOpenForWriting(const std::vector<std::filesystem::path>& paths);

// `dest`, or "<dest> (2)" and so on when something is already there.
std::filesystem::path FreeName(const std::filesystem::path& dest);

struct ArchiveEntry {
  std::filesystem::path path;
  std::uintmax_t size = 0;  // 0 for a tarball's: listing it doesn't say
};
struct ArchiveListing {
  std::vector<ArchiveEntry> entries;    // files only; none when the names are encrypted too
  std::optional<std::string> password;  // FindPassword's answer
};
// What's in `archive`, opened by one of `candidates` when it's encrypted.
Result<ArchiveListing> ListArchive(const std::filesystem::path& archive,
                                   const std::vector<std::string>& candidates);

// Which of `candidates` opens `archive`: empty when it isn't encrypted, nullopt when none does.
Result<std::optional<std::string>> FindPassword(const std::filesystem::path& archive,
                                                const std::vector<std::string>& candidates);

// Deletes the links under `dir` that lead out of it, or nowhere: an archive could aim one at
// anything on the system.
void RemoveEscapingLinks(const std::filesystem::path& dir);

std::filesystem::path StemWithoutArchiveExtension(const std::filesystem::path& archive);

// Name prefix of the hidden folders archives are extracted in.
inline constexpr std::string_view kExtractingPrefix = ".mira-extracting-";

}  // namespace mira::library
