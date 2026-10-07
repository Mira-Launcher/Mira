#pragma once

#include <filesystem>
#include <string_view>

#include "core/Result.h"

namespace mira {

// Writes `content` beside `path` and renames it over, so a failed write (a full
// disk included) never replaces a working file with a truncated one. Creates
// the parent folder. `code` names the error on failure.
// Writes through a temp file and rename. `durable` fsyncs before the rename.
Result<void> WriteFileAtomic(const std::filesystem::path& path, std::string_view content, std::string code,
                             bool durable = false);

// Renames an unreadable `path` to the first free <path>.bad, <path>.bad.2, ... so an earlier
// set-aside copy is never overwritten. Returns the new name.
Result<std::filesystem::path> SetAside(const std::filesystem::path& path);

// The error a save returns instead of overwriting a file SetAside couldn't move.
std::unexpected<Error> KeptFileError(const std::filesystem::path& path);

}  // namespace mira
