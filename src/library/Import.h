#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "config/Config.h"
#include "core/Result.h"

namespace mira::library {

enum class ImportKind { kUnknown, kGame, kApp };

struct ImportGuess {
  ImportKind kind = ImportKind::kUnknown;
  std::string name;    // cleaned display name
  std::string reason;  // short human reason, e.g. "AppImage category Game", "Steam lists it as software"
};

// What a dropped file or folder is. May run an AppImage's own extractor and ask Steam's store.
ImportGuess ClassifyImport(const std::filesystem::path& path);

// Moves `path` into the Applications root (kApp) or the first other library root (kGame). Returns the new path.
Result<std::filesystem::path> ImportInto(const config::Config& config, const std::filesystem::path& path,
                                         ImportKind kind);

std::string_view ImportKindName(ImportKind kind);  // "game", "app", "unknown"
std::optional<ImportKind> ParseImportKind(std::string_view text);  // "game"/"app" only

}  // namespace mira::library
