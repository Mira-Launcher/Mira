#include "runner/NativeRunner.h"

#include <cctype>
#include <filesystem>
#include <format>
#include <vector>

#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

// Only meaningful for a file that actually exists: a nonexistent exe_path
// is left to fail at
// exec time as before, not reported as "not executable".
bool MissingExecuteBit(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::status(path, ec);
  if (ec || !fs::exists(status)) return false;
  return (status.permissions() & fs::perms::owner_exec) == fs::perms::none;
}

std::string ToLower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// A FUSE-less system needs "--appimage-extract-and-run" as the AppImage's
// own first argument (see AppImage's own docs): /dev/fuse absent, or
// neither fusermount nor fusermount3 on PATH, both mean no FUSE.
bool HasFuse() {
  std::error_code ec;
  return std::filesystem::exists("/dev/fuse", ec) && (FindOnPath("fusermount") || FindOnPath("fusermount3"));
}

// Downloaded AppImages routinely lose their executable bit (a browser
// download strips it) -- every AppImage-aware launcher chmods it before
// running rather than erroring out over something this routine to fix.
// Best-effort: MissingExecuteBit below still catches a filesystem that
// won't allow it (read-only, no permission).
void MakeExecutable(const fs::path& path) {
  std::error_code ec;
  const auto current = fs::status(path, ec).permissions();
  if (ec) return;
  fs::permissions(path, current | fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, ec);
}

}  // namespace

Result<Command> NativeRunner::BuildCommand(const model::Game& game,
                                            const std::optional<model::RunnerBuild>&) const {
  if (game.exe_path.empty()) return NoExecutable(game);

  const std::filesystem::path install_path = game.install_path;
  const std::filesystem::path exe = install_path / game.exe_path;
  const std::string ext = ToLower(exe.extension().string());

  Command command;
  if (ext == ".sh" || ext == ".bash") {
    // A .sh routinely loses its executable bit in an archive, so it's still
    // accepted as a candidate without one (see library::Detector), so run it
    // through sh explicitly instead of relying on exec's own +x check.
    command.argv = {"sh", exe.string()};
  } else if (ext == ".appimage") {
    // Downloaded AppImages routinely lose their executable bit too (a
    // browser download strips it) -- an AppImage execs itself either way
    // (--appimage-extract-and-run is still argv[0] = the AppImage, just
    // telling its own embedded runtime not to need FUSE), so the bit is
    // required regardless of which branch below runs.
    if (MissingExecuteBit(exe)) MakeExecutable(exe);
    if (MissingExecuteBit(exe)) {
      return Err("not_executable",
                std::format("\"{}\" isn't allowed to run, and Mira couldn't change that", exe.string()),
                "Make the file executable (chmod +x), or check the folder isn't read-only.");
    }
    command.argv = HasFuse() ? std::vector<std::string>{exe.string()}
                             : std::vector<std::string>{exe.string(), "--appimage-extract-and-run"};
  } else {
    if (MissingExecuteBit(exe)) {
      return Err("not_executable", std::format("\"{}\" isn't marked as a program that can run", exe.string()),
                "Make it executable (chmod +x), or choose the game's real launcher instead.",
                Fix::Game(game.id, "exe"));
    }
    command.argv.push_back(exe.string());
  }

  for (const std::string& arg : strings::SplitArgs(game.args)) {
    if (!arg.empty()) command.argv.push_back(arg);
  }
  command.env = game.env;
  command.cwd = game.working_dir.empty() ? exe.parent_path() : install_path / game.working_dir;
  return command;
}

}  // namespace mira::runner
