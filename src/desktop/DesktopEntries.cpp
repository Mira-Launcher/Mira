#include "core/Json.h"
#include "desktop/DesktopEntries.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <set>

#include <json.hpp>

#include "config/Resolver.h"
#include "core/AtomicFile.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "launchers/Office.h"
#include "metadata/MetadataFetcher.h"
#include "runner/Exec.h"
#include "setup/Setup.h"

namespace mira::desktop {
namespace {
namespace fs = std::filesystem;

constexpr std::string_view kPrefix = "mira-";
constexpr std::string_view kSuffix = ".desktop";

// Freedesktop values are newline-delimited, so a name containing one would
// inject arbitrary keys into the entry.
std::string Sanitize(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c != '\n' && c != '\r') out += c;
  }
  return out;
}

// Absolute path to a game's cached cover art, if any is actually on disk --
// a .desktop file's Icon= accepts an absolute path just as well as a themed
// icon name, per spec, so a game with real artwork doesn't have to settle
// for the generic fallback.
std::optional<fs::path> CachedArtwork(const config::Config& config, const std::string& game_id) {
  std::ifstream meta_in(metadata::MetadataFile(config, game_id));
  if (!meta_in) return std::nullopt;
  const nlohmann::json info = nlohmann::json::parse(meta_in, nullptr, false);
  if (info.is_discarded() || !info.contains("artwork")) return std::nullopt;
  const std::string name = core::JsonString(info["artwork"], "file");
  if (name.empty()) return std::nullopt;  // otherwise the artwork directory itself
  const fs::path file = metadata::ArtworkDir(config, game_id) / name;
  return std::ifstream(file, std::ios::binary).good() ? std::make_optional(file) : std::nullopt;
}

// `mira` or `mira-gui` by full path, since a desktop session's PATH often lacks ~/.local/bin: from an
// AppImage, the wrapper `mira setup` writes and the AppImage itself; from a package, the one on PATH
// (/usr/bin's scripts set up its environment); otherwise the one next to mirad.
std::string Binary(std::string_view name) {
  std::error_code ec;
  fs::path found;
  if (const char* appimage = std::getenv("APPIMAGE"); appimage != nullptr && *appimage != '\0') {
    found = name == "mira-gui" ? fs::path(appimage) : setup::DefaultPaths().bin_dir / name;
  } else if (const auto on_path = runner::FindOnPath(name); on_path && on_path->starts_with('/')) {
    found = *on_path;
  } else {
    found = fs::read_symlink("/proc/self/exe", ec).parent_path() / name;
  }
  if (ec || !fs::is_regular_file(found, ec)) return std::string(name);
  return found.string().contains(' ') ? std::format("\"{}\"", found.string()) : found.string();
}

// The file types a Microsoft 365 app opens; empty for anything else.
std::string_view MimeTypes(const model::Game& game) {
  if (game.source != "office") return {};
  for (const auto& app : launchers::office::Apps()) {
    if (app.ref == game.source_ref) return app.mime;
  }
  return {};
}

// The file types a Microsoft 365 app opens, and the desktop's launch feedback while it starts: its window's
// class is Proton's for the GAMEID ProtonRunner gives it ("steam_app_mira_officeword").
std::string OfficeAppKeys(const model::Game& game, std::string_view mime) {
  std::string window_class = "steam_app_mira_";
  for (const char ch : game.id) {
    if (std::isalnum(static_cast<unsigned char>(ch))) window_class.push_back(ch);
  }
  return std::format("MimeType={}\nStartupNotify=true\nStartupWMClass={}\n", mime, window_class);
}

// So file managers offer the apps for their file types.
void UpdateMimeCache(const fs::path& dir) {
  Command command;
  command.argv = {"update-desktop-database", dir.string()};
  if (auto ran = runner::RunAndWait(command); !ran || ran->exit_code != 0) log::Info("update-desktop-database didn't run");
}

std::optional<std::string> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

DesktopEntries::DesktopEntries(config::Config& config) : config_(config) {}

fs::path DesktopEntries::EntryPath(const std::string& game_id) const {
  return config_.GetPath("desktop_entries.directory") /
         std::format("{}{}{}", kPrefix, game_id, kSuffix);
}

bool DesktopEntries::IsWanted(const model::Game& game) const {
  if (game.status != model::GameStatus::Ready) return false;
  // Steam already puts every game in your Steam library into the desktop
  // menu itself (via its own Linux integration), a second, Mira-owned
  // entry for the same game is redundant clutter, not a missing feature,
  // regardless of steam.launch_mode. Sync() below removes any mira-<id>
  // entry that stops being "wanted", so this also cleans up an entry a
  // Steam game already had from before this exclusion existed.
  if (game.runner_ref.starts_with("steam:")) return false;
  if (game.exe_path.empty()) return false;
  // Microsoft 365 itself has nothing to open; its apps have entries of their own.
  if (game.source == "launcher" && game.source_ref == "office") return false;

  const config::Resolver resolver(config_, game.overrides);
  return resolver.GetBool("desktop_entries.enabled");
}

std::string DesktopEntries::Render(const model::Game& game) const {
  const config::Resolver resolver(config_, game.overrides);
  const std::string_view mime = MimeTypes(game);
  // An app that opens files is handed them, which only the CLI passes on.
  const std::string exec = !mime.empty() ? std::format("{} launch {} %F", Binary("mira"), game.id)
                           : resolver.GetString("desktop_entries.exec_mode") == "frontend"
                               ? std::format("{} --launch {}", Binary("mira-gui"), game.id)
                               : std::format("{} launch {}", Binary("mira"), game.id);
  const std::optional<fs::path> artwork = CachedArtwork(config_, game.id);
  const bool app = std::ranges::contains(game.tags, "app");
  const std::string icon = artwork ? artwork->string() : app ? "application-x-executable" : "applications-games";
  const std::string categories = app ? "Utility;" : resolver.GetString("desktop_entries.categories");

  return std::format(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name={}\n"
      "Comment=Launch {} with Mira\n"
      "Exec={}\n"
      "Icon={}\n"
      "Categories={}\n"
      "Terminal=false\n"
      "X-Mira-Game-Id={}\n"
      "{}",
      Sanitize(game.name), Sanitize(game.name), exec, icon,
      Sanitize(categories), game.id, mime.empty() ? "" : OfficeAppKeys(game, mime));
}

Result<void> DesktopEntries::SyncOne(const std::string& game_id, const std::optional<model::Game>& game) {
  const fs::path path = EntryPath(game_id);
  if (!game || !config_.GetBool("desktop_entries.enabled") || !IsWanted(*game)) {
    std::error_code ec;
    fs::remove(path, ec);
    return {};
  }
  const std::string content = Render(*game);
  if (ReadFile(path) == content) return {};  // every write makes the desktop re-index its menu
  if (auto written = WriteFileAtomic(path, content, "desktop_write_failed"); !written) return written;
  if (!MimeTypes(*game).empty()) UpdateMimeCache(path.parent_path());
  return {};
}

Result<void> DesktopEntries::Sync(const std::vector<model::Game>& games) {
  // The global switch still gates everything -- a per-game override can
  // exclude one game while the rest of the menu stays on, but it can't turn
  // entries back on when they're off globally.
  const bool enabled = config_.GetBool("desktop_entries.enabled");
  const fs::path dir = config_.GetPath("desktop_entries.directory");

  std::error_code ec;
  if (enabled) {
    fs::create_directories(dir, ec);
    if (ec) return Err("desktop_dir_failed", ec.message());
  } else if (!fs::is_directory(dir, ec)) {
    return {};  // disabled and nothing was ever written
  }

  // Which entries should exist now. Disabled means none, which also cleans
  // up anything written while it was on.
  std::set<std::string> wanted;
  if (enabled) {
    for (const model::Game& game : games) {
      if (IsWanted(game)) wanted.insert(game.id);
    }
  }

  bool mime_changed = false;
  for (const model::Game& game : games) {
    if (!wanted.contains(game.id)) continue;
    const fs::path path = EntryPath(game.id);
    // Unchanged entries are left alone: every write makes the desktop re-index its menu.
    const std::string content = Render(game);
    if (ReadFile(path) == content) continue;
    if (auto written = WriteFileAtomic(path, content, "desktop_write_failed"); !written) {
      log::Warn("could not write desktop entry {}: {}", path.string(), written.error().message);
    } else if (!MimeTypes(game).empty()) {
      mime_changed = true;
    }
  }
  if (mime_changed) UpdateMimeCache(dir);

  // Remove ours that are no longer wanted: a game deleted, gone missing, or
  // now needing an install. Anything not named mira-<id>.desktop is left
  // strictly alone.
  for (const fs::path& entry : paths::ListDir(dir)) {
    const std::string name = entry.filename().string();
    if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix)) continue;
    const std::string id = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
    if (wanted.contains(id)) continue;
    fs::remove(entry, ec);
  }
  return {};
}

}  // namespace mira::desktop
