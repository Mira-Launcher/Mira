#include "setup/Setup.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

#include "core/AtomicFile.h"
#include "core/Paths.h"

namespace mira::setup {
namespace {
namespace fs = std::filesystem;

constexpr std::string_view kMarker = "# Written by `mira setup`";
// The hand-written wrapper this replaces (same behavior, no marker).
constexpr std::string_view kLegacyMarker = "Wrapper: always run whichever binary";
constexpr const char* kLinks[] = {"mirad", "mira-run"};

fs::path EnvOr(const char* name, const fs::path& fallback) {
  const char* value = std::getenv(name);
  return value && *value ? fs::path(value) : fallback;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool IsOurs(const fs::path& path) {
  const std::string content = ReadFile(path);
  return content.find(kMarker) != std::string::npos || content.find(kLegacyMarker) != std::string::npos;
}

Result<void> WriteFile(const fs::path& path, const std::string& content) {
  return WriteFileAtomic(path, content, "setup_write_failed");
}

// Runs the bundled binary named like $0, re-extracting the AppImage only when
// it changed. APPIMAGE is exported so a later `mira setup` still finds it.
std::string Wrapper(const fs::path& appimage) {
  return std::format(R"sh(#!/bin/sh
{}: runs the binaries bundled in the Mira AppImage.
set -e
APPIMAGE="{}"
CACHE_DIR="${{XDG_CACHE_HOME:-$HOME/.cache}}/mira-appimage"
BIN_NAME=$(basename "$0")

if [ ! -f "$APPIMAGE" ]; then
  echo "mira: $APPIMAGE not found -- run \`mira setup\` from the AppImage again" >&2
  exit 1
fi

STAMP="$CACHE_DIR/.extracted-mtime"
CURRENT_MTIME=$(stat -c %Y "$APPIMAGE")
mkdir -p "$CACHE_DIR"
# One process extracts at a time, and into a folder of its own that then replaces squashfs-root, so none
# runs from a half-extracted tree.
exec 9>"$CACHE_DIR/.lock"
if command -v flock >/dev/null 2>&1; then flock 9; fi
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$CURRENT_MTIME" ]; then
  WORK_DIR=$(mktemp -d "$CACHE_DIR/extract.XXXXXX")
  ( cd "$WORK_DIR" && "$APPIMAGE" --appimage-extract >/dev/null )
  rm -rf "$CACHE_DIR/squashfs-root"
  mv "$WORK_DIR/squashfs-root" "$CACHE_DIR/squashfs-root"
  rm -rf "$WORK_DIR"
  echo "$CURRENT_MTIME" > "$STAMP"
fi
exec 9>&-

export APPIMAGE
exec "$CACHE_DIR/squashfs-root/usr/bin/$BIN_NAME" "$@"
)sh",
                     kMarker, appimage.string());
}

std::string DesktopEntry(const fs::path& appimage) {
  return std::format(R"([Desktop Entry]
{1}
Type=Application
Name=Mira
Comment=Launch and manage native and Windows games
Exec="{0}"
Icon=mira
Categories=Game;
Terminal=false
Actions=uninstall;

[Desktop Action uninstall]
Name=Uninstall Mira
Icon=edit-delete
Exec="{0}" setup --uninstall
)",
                     appimage.string(), kMarker);
}

std::string Unit(const fs::path& mirad) {
  return std::format(R"({}
[Unit]
Description=Mira game launcher backend
After=graphical-session.target

[Service]
Type=simple
ExecStart={}
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
)",
                     kMarker, mirad.string());
}

fs::path IconPath(const SetupPaths& paths) { return paths.icons_dir / "hicolor/256x256/apps/mira.png"; }
fs::path DesktopPath(const SetupPaths& paths) { return paths.applications_dir / "mira.desktop"; }
fs::path UnitPath(const SetupPaths& paths) { return paths.systemd_dir / "mirad.service"; }

}  // namespace

SetupPaths DefaultPaths() {
  const fs::path home = paths::Home();
  const fs::path data = EnvOr("XDG_DATA_HOME", home / ".local/share");
  return {
      .bin_dir = home / ".local/bin",
      .applications_dir = data / "applications",
      .icons_dir = data / "icons",
      .systemd_dir = EnvOr("XDG_CONFIG_HOME", home / ".config") / "systemd/user",
  };
}

Result<std::vector<fs::path>> Install(const SetupPaths& paths, const fs::path& appimage, const fs::path& icon) {
  std::error_code ec;
  if (!fs::is_regular_file(appimage, ec)) {
    return Err("appimage_missing", std::format("no AppImage at {}", appimage.string()));
  }
  const fs::path wrapper = paths.bin_dir / "mira";
  if (fs::exists(fs::symlink_status(wrapper, ec)) && !IsOurs(wrapper)) {
    return Err("setup_conflict", std::format("{} exists and wasn't written by mira setup", wrapper.string()));
  }

  std::vector<fs::path> written;
  if (auto ok = WriteFile(wrapper, Wrapper(fs::absolute(appimage))); !ok) return std::unexpected(ok.error());
  fs::permissions(wrapper, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                               fs::perms::others_read | fs::perms::others_exec, ec);
  written.push_back(wrapper);

  for (const char* name : kLinks) {
    const fs::path link = paths.bin_dir / name;
    const fs::file_status status = fs::symlink_status(link, ec);
    if (fs::exists(status) && !fs::is_symlink(status)) {
      return Err("setup_conflict", std::format("{} exists and isn't a symlink", link.string()));
    }
    fs::remove(link, ec);
    fs::create_symlink("mira", link, ec);
    if (ec) return Err("setup_write_failed", std::format("couldn't link {}: {}", link.string(), ec.message()));
    written.push_back(link);
  }

  if (auto ok = WriteFile(DesktopPath(paths), DesktopEntry(fs::absolute(appimage))); !ok) {
    return std::unexpected(ok.error());
  }
  written.push_back(DesktopPath(paths));

  if (fs::is_regular_file(icon, ec)) {
    fs::create_directories(IconPath(paths).parent_path(), ec);
    fs::copy_file(icon, IconPath(paths), fs::copy_options::overwrite_existing, ec);
    if (!ec) written.push_back(IconPath(paths));
  }

  if (auto ok = WriteFile(UnitPath(paths), Unit(paths.bin_dir / "mirad")); !ok) return std::unexpected(ok.error());
  written.push_back(UnitPath(paths));
  return written;
}

Result<std::vector<fs::path>> Remove(const SetupPaths& paths) {
  std::error_code ec;
  std::vector<fs::path> removed;
  const auto remove_if = [&](const fs::path& path, bool ours) {
    if (ours && fs::remove(path, ec)) removed.push_back(path);
  };

  const fs::path wrapper = paths.bin_dir / "mira";
  const bool wrapper_ours = fs::exists(wrapper, ec) && IsOurs(wrapper);
  for (const char* name : kLinks) {
    const fs::path link = paths.bin_dir / name;
    remove_if(link, wrapper_ours && fs::is_symlink(fs::symlink_status(link, ec)) && fs::read_symlink(link, ec) == "mira");
  }
  remove_if(wrapper, wrapper_ours);
  remove_if(DesktopPath(paths), fs::exists(DesktopPath(paths), ec) && IsOurs(DesktopPath(paths)));
  remove_if(UnitPath(paths), fs::exists(UnitPath(paths), ec) && IsOurs(UnitPath(paths)));
  if (!removed.empty()) remove_if(IconPath(paths), fs::exists(IconPath(paths), ec));
  return removed;
}

}  // namespace mira::setup
