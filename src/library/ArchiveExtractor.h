#pragma once

#include <filesystem>
#include <optional>
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

std::filesystem::path StemWithoutArchiveExtension(const std::filesystem::path& archive);

// Extracts into a hidden folder next to `dest_dir` and moves the result into
// place only once the tool succeeds, so a failed or truncated archive never
// leaves a partial game behind. Every volume is deleted on success; on
// failure the archive is kept. A folder already at `dest_dir` is kept too: the archive goes in
// "<dest_dir> (2)".
Result<void> ExtractAndRemove(const std::filesystem::path& archive,
                              const std::filesystem::path& dest_dir);

// Name prefix of the hidden folders ExtractAndRemove works in.
inline constexpr std::string_view kExtractingPrefix = ".mira-extracting-";

}  // namespace mira::library
