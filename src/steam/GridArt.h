#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "core/Result.h"

namespace mira::steam {

// Art for one Steam app by Mira's slot ("cover", "hero", "logo", "icon"): PNG or JPEG files.
using GridPicks = std::map<std::string, std::filesystem::path>;

// Makes every Steam account's config/grid/ show `picks` as `appid`'s art, as Steam's own "Set
// custom artwork" does; a slot left out goes back to what it was before Mira wrote it. Only files
// listed in `written` (Mira's own, read and saved here) are replaced or removed: art set in Steam
// itself is kept as <file>.before-mira and put back then. Steam shows changes after it restarts.
Result<void> SyncGridArt(const std::filesystem::path& steam_root, const std::string& appid,
                         const GridPicks& picks, const std::filesystem::path& written);

// Takes away every file SyncGridArt wrote, putting back what each replaced.
Result<void> RemoveGridArt(const std::filesystem::path& steam_root,
                           const std::filesystem::path& written);

}  // namespace mira::steam
