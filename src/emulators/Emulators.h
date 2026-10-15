#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "model/Types.h"
#include "runner/Downloader.h"
#include "store/GameStore.h"

// Standalone emulators. Each one is a source: its games are the ROMs in its own folder under
// emulators.roms_root, which the user adds, and they launch through the emulator runner.
namespace mira::emulators {

// What the user supplies for the emulator to run: a BIOS, firmware or keys. Mira never downloads
// these.
struct Firmware {
  std::string id = {};
  std::string label = {};
  std::string dir = {};  // where it goes, "~" for home
  std::vector<std::string> patterns =
      {};  // file names it accepts; any match there means it's present
  bool required = true;
  std::string install_flag = {};  // the emulator installs it itself: `<emulator> <flag> <file>`
};

// Where an emulator's own settings list its game folders: the paths `pattern`'s first group matches
// in `file`, one line at a time, split at `separator` when it has one.
struct KnownDirs {
  std::string file;  // "~" for home
  std::string pattern;
  char separator = 0;
};

struct Emulator {
  std::string id = {};  // also its games' `source`
  std::string name = {};
  std::vector<std::string> consoles = {};    // short names: "ps1", "snes"
  std::vector<std::string> extensions = {};  // lower case, with the dot
  // A folder is one game when this file is inside it (PS3's PS3_GAME/USRDIR/EBOOT.BIN); empty:
  // games are files.
  std::string folder_game = {};
  // Every folder in its games folder is one game, started by its likeliest file (DOSBox's,
  // ScummVM's); loose files it takes are games too.
  bool folders = false;
  std::vector<std::string> args = {
      "{rom}"};  // "{rom}" is the game's file, "{dir}" its folder, "{stem}" its name
  std::vector<std::string> fullscreen = {};  // before `args`, with emulators.fullscreen
  std::vector<std::string> open_args =
      {};  // opening it alone: shadPS4's own window is its Big Picture mode
  std::string open_binary =
      {};  // opening it alone runs this file beside the binary instead (KytyPS5's launcher)
  // Seconds Stop waits before SIGKILL; 0 is launch.stop_timeout_s. Dolphin's batch mode never quits
  // on SIGTERM.
  std::int64_t stop_timeout_s = 0;
  // Where it comes from: the release at `release_url` (a GitHub, Forgejo or GitLab API url) whose
  // asset matches `asset`, the newest file matching `asset` in the folder listing at `listing_url`
  // (a CachyOS package), the one file at `download_url`, or else the system package of the
  // system::Packages feature named by its id.
  std::string release_url = {};
  std::string listing_url = {};
  std::string asset = {};
  std::vector<std::string> exclude = {};  // asset names with these in them are passed over
  std::string download_url = {};
  std::string binary = {};  // the installed file's name, or the package's program
  std::vector<Firmware> firmware = {};
  // Settings files written before it first runs, where none is ("~" for home), so its first-run
  // questions (a setup wizard, a desktop shortcut) don't stand between a game and the screen.
  std::vector<std::pair<std::string, std::string>> prefill = {};
  std::vector<std::pair<std::string, std::string>> env = {};  // set when it runs
  std::vector<std::string> data =
      {};  // what it creates for itself: settings, saves, caches ("~" for home)
  std::vector<KnownDirs> known_dirs = {};
};

const std::vector<Emulator>& All();
const Emulator* Find(std::string_view id);
const Emulator* ForGame(const model::Game& game);
// Whether Mira downloads it, rather than the system's packages installing it.
bool Downloads(const Emulator& emulator);

std::filesystem::path RomDir(const config::Config& config, const Emulator& emulator);
// Its games folder, then the folders of games kept where they are (emulators.<id>.game_dirs).
std::vector<std::filesystem::path> GameRoots(const config::Config& config,
                                             const Emulator& emulator);
// A game is its whole folder, not one file.
bool ByFolder(const Emulator& emulator);
// The game's file: its ROM, or the file its folder starts with.
std::filesystem::path MainFile(const model::Game& game);
// A file straight in its emulator's folder, not a folder game.
bool IsLooseFile(const config::Config& config, const model::Game& game);

// Where the emulator runs from: the emulators.<id>.bin setting, Mira's download, then $PATH. Empty
// when none.
std::filesystem::path Binary(const config::Config& config, const Emulator& emulator);
// Its `data` that's on disk: settings, saves and caches, deleted only when asked.
std::vector<std::filesystem::path> DataPaths(const Emulator& emulator);
// Mira's download of it, empty when there's none.
std::filesystem::path DownloadDir(const config::Config& config, const Emulator& emulator);
// The release tag of Mira's download, empty for any other copy.
std::string InstalledVersion(const config::Config& config, const Emulator& emulator);

// Writes the emulator's prefill files that aren't there yet.
void Prefill(const Emulator& emulator);

// Where it downloads from by default: "owner/repo" on GitHub, else the releases API, listing or
// file link.
std::string DefaultSource(const Emulator& emulator);
// The release Install would download, from emulators.<id>.repo; only for an emulator Mira
// downloads.
Result<runner::ReleaseAsset> Latest(const config::Config& config, const Emulator& emulator);
// Downloads the latest release over Mira's copy. A package emulator returns the packages error
// instead.
Result<std::filesystem::path> Install(const config::Config& config, const Emulator& emulator);

struct FirmwareState {
  const Firmware* firmware;
  bool present = false;
};
std::vector<FirmwareState> FirmwareStatus(const Emulator& emulator);
// Puts `files` where the emulator looks: a zip of Switch firmware is unpacked, PS3 and Vita
// firmware installed by the emulator. A file of the same name already there is refused
// (already_there) unless `replace`.
Result<void> AddFirmware(const config::Config& config, const Emulator& emulator,
                         const Firmware& firmware, const std::vector<std::filesystem::path>& files,
                         bool replace);

// A ROM's name without its extension and dump tags: "Metroid Fusion (USA) [!].gba" reads "Metroid
// Fusion".
std::string CleanName(std::string_view file_name);
// The files a `folders` emulator's game could start with, when Mira can't tell (none is named like
// its folder); empty when it can.
std::vector<std::string> StartChoices(const config::Config& config, const model::Game& game);
// The game's files: its ROM and those a .cue, .gdi or .m3u names, or its folder.
std::vector<std::filesystem::path> GameFiles(const config::Config& config, const model::Game& game);

// Whether games added to it are moved rather than copied: emulators.<id>.add_mode, else
// emulators.move_added.
bool MovesAdded(const config::Config& config, const Emulator& emulator);
// Copies (or moves) `files` into the emulator's folder with the files they name, then imports it.
Result<library::ImportSummary> AddFiles(config::Config& config, store::GameStore& games,
                                        api::EventBus& events, const Emulator& emulator,
                                        const std::vector<std::filesystem::path>& files, bool move);

// Games it had before Mira did: in the folders its own settings list, and desktop entries in the
// library that run one with it.
struct ExistingGames {
  struct Dir {
    std::filesystem::path path;
    int games = 0;
  };
  struct Shortcut {
    std::string id;  // the desktop entry's game
    std::string name;
    std::filesystem::path file;
  };
  std::vector<Dir> dirs;
  std::vector<Shortcut> shortcuts;
};
ExistingGames FindExisting(const config::Config& config, const store::GameStore& games,
                           const Emulator& emulator);
// Makes them its games, keeping a shortcut's id, playtime and art: moved into its folder with
// `move`, else tracked where they are.
Result<library::ImportSummary> AdoptExisting(config::Config& config, store::GameStore& games,
                                             api::EventBus& events, const Emulator& emulator,
                                             bool move);

// What it runs with: its own variables, and its AppImage's self-updater turned off.
std::map<std::string, std::string> Environment(const Emulator& emulator);

// The emulator an AppImage found in a library folder is, by its file name; null for any other
// program.
const Emulator* Recognize(const std::filesystem::path& appimage);
// Makes `appimage` the emulator's program when it has none and adds its source, so it opens from
// its page instead of being a game.
void Adopt(config::Config& config, store::GameStore& games, api::EventBus& events,
           const Emulator& emulator, const std::filesystem::path& appimage);

// The other installed emulators `game` could run with: sharing a console and taking its file.
std::vector<const Emulator*> Alternatives(const config::Config& config, const model::Game& game);
// Moves the game's files into `to`'s folder; it keeps its id, art, tags and playtime.
Result<model::Game> MoveGame(config::Config& config, store::GameStore& games,
                             const model::Game& game, const Emulator& to);

Result<library::ImportSummary> Import(config::Config& config, store::GameStore& games,
                                      api::EventBus& events, const Emulator& emulator);
// Imports from every added emulator source; used by scans.
library::ImportSummary ImportAll(config::Config& config, store::GameStore& games,
                                 api::EventBus& events);

}  // namespace mira::emulators
