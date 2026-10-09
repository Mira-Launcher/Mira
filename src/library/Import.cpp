#include "library/Import.h"

#include <stdlib.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

#include "core/Command.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/Relocate.h"
#include "metadata/MetadataFetcher.h"
#include "runner/Exec.h"

namespace mira::library {
namespace fs = std::filesystem;
namespace {

std::string Lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::tolower(c); });
  return text;
}

// The name a dropped path goes by: a file's stem (two for .tar.gz), a folder's own name.
std::string NameOf(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return path.filename().string();
  fs::path stem = path.stem();
  if (path.extension() == ".gz" && stem.extension() == ".tar") stem = stem.stem();
  return stem.string();
}

// The Categories line of the AppImage's own desktop entry; nullopt when it can't be extracted or has none.
std::optional<std::string> AppImageCategories(const fs::path& path) {
  std::string templ = (fs::temp_directory_path() / "mira-import-XXXXXX").string();
  if (!::mkdtemp(templ.data())) return std::nullopt;
  const fs::path dir = templ;

  Command command;
  command.argv = {path.string(), "--appimage-extract", "*.desktop"};
  command.cwd = dir;
  command.timeout_s = 20;
  const Result<runner::ExecResult> run = runner::RunAndWait(command);
  std::optional<std::string> categories;
  if (run && run->exit_code == 0) {
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir / "squashfs-root", ec)) {
      if (entry.path().extension() != ".desktop") continue;
      std::ifstream file(entry.path());
      for (std::string line; std::getline(file, line);) {
        if (line.starts_with("Categories=")) {
          categories = line.substr(std::string_view("Categories=").size());
          break;
        }
      }
      break;
    }
  }
  fs::remove_all(dir);
  return categories;
}

// Whether `root` is the Applications folder.
bool IsApplicationsRoot(const fs::path& root) {
  return Lower(root.filename().string()) == "applications";
}

}  // namespace

ImportGuess ClassifyImport(const fs::path& dropped) {
  const fs::path path = dropped.has_filename() ? dropped : dropped.parent_path();
  ImportGuess guess;
  guess.name = strings::CleanGameName(NameOf(path));

  std::error_code ec;
  if (fs::is_regular_file(path, ec) && Lower(path.extension().string()) == ".appimage") {
    if (const std::optional<std::string> categories = AppImageCategories(path)) {
      if (categories->find("Game") != std::string::npos) {
        guess.kind = ImportKind::kGame;
        guess.reason = "AppImage category Game";
      } else {
        guess.kind = ImportKind::kApp;
        guess.reason = "AppImage categories: " + *categories;
      }
      return guess;
    }
  }

  if (const std::optional<bool> software = metadata::SteamSaysSoftware(guess.name)) {
    guess.kind = *software ? ImportKind::kApp : ImportKind::kGame;
    guess.reason = *software ? "Steam lists it as software" : "Steam lists it as a game";
  } else {
    guess.reason = "Not found on Steam";
  }
  return guess;
}

Result<fs::path> ImportInto(const config::Config& config, const fs::path& dropped, ImportKind kind) {
  const fs::path path = dropped.has_filename() ? dropped : dropped.parent_path();
  std::error_code ec;
  if (!fs::exists(path, ec)) return Err("source_missing", std::format("{} doesn't exist", path.string()));
  if (kind != ImportKind::kGame && kind != ImportKind::kApp) {
    return Err("invalid_kind", "kind must be \"game\" or \"app\"");
  }

  const std::vector<fs::path> roots = config.GetPathArray("library_roots");
  const bool apps = kind == ImportKind::kApp;
  const auto root = std::ranges::find_if(roots, [&](const fs::path& r) { return IsApplicationsRoot(r) == apps; });
  if (root == roots.end()) {
    return Err("no_library_root", apps ? "No Applications folder in library_roots" : "No games folder in library_roots");
  }
  if (paths::IsWithin(path, roots)) {
    return Err("already_in_library", std::format("{} is already in the library", path.string()));
  }

  const fs::path target = *root / path.filename();
  if (fs::exists(target, ec)) return Err("target_exists", std::format("{} already exists", target.string()));
  // From another drive it's copied beside the root first, so the watcher never sees half of it.
  const fs::path staging = root->parent_path() / ".mira-import" / path.filename();
  fs::remove_all(staging, ec);
  if (auto moved = MovePath(path, staging, /*allow_copy=*/true); !moved) return std::unexpected(moved.error());
  fs::create_directories(*root, ec);
  fs::rename(staging, target, ec);
  if (ec) return Err("move_failed", std::format("{} is in {}, but couldn't go into the library: {}", path.filename().string(),
                                                staging.parent_path().string(), ec.message()));
  fs::remove(staging.parent_path(), ec);
  return target;
}

std::string_view ImportKindName(ImportKind kind) {
  switch (kind) {
    case ImportKind::kGame: return "game";
    case ImportKind::kApp: return "app";
    case ImportKind::kUnknown: return "unknown";
  }
  return "unknown";
}

std::optional<ImportKind> ParseImportKind(std::string_view text) {
  if (text == "game") return ImportKind::kGame;
  if (text == "app") return ImportKind::kApp;
  return std::nullopt;
}

}  // namespace mira::library
