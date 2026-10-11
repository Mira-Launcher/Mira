#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "model/Types.h"
#include "store/GameStore.h"

// Standalone emulators. Each one is a source: its games are the ROMs in its own folder under
// emulators.roms_root, which the user adds, and they launch through the emulator runner.
namespace mira::emulators {

// What the user supplies for the emulator to run: a BIOS, firmware or keys. Mira never downloads these.
struct Firmware {
  std::string id = {};
  std::string label = {};
  std::string dir = {};                    // where it goes, "~" for home
  std::vector<std::string> patterns = {};  // file names it accepts; any match there means it's present
  bool required = true;
};

struct Emulator {
  std::string id = {};  // also its games' `source`
  std::string name = {};
  std::vector<std::string> consoles = {};    // short names: "ps1", "snes"
  std::vector<std::string> extensions = {};  // lower case, with the dot
  // A folder is one game when this file is inside it (PS3's PS3_GAME/USRDIR/EBOOT.BIN); empty: games are files.
  std::string folder_game = {};
  std::vector<std::string> args = {};  // "{rom}" is the game's file
  // Where it comes from: the latest release at `release_url` (a GitHub or Forgejo API url) whose asset
  // matches `asset`, or else the system package of the system::Packages feature named by its id.
  std::string release_url = {};
  std::string asset = {};
  std::string binary = {};  // the installed file's name, or the package's program
  std::vector<Firmware> firmware = {};
};

const std::vector<Emulator>& All();
const Emulator* Find(std::string_view id);
const Emulator* ForGame(const model::Game& game);

std::filesystem::path RomDir(const config::Config& config, const Emulator& emulator);

// Where the emulator runs from: the emulators.<id>.bin setting, Mira's download, then $PATH. Empty when none.
std::filesystem::path Binary(const config::Config& config, const Emulator& emulator);
// The release tag of Mira's download, empty for any other copy.
std::string InstalledVersion(const config::Config& config, const Emulator& emulator);

// Downloads the latest release over Mira's copy. A package emulator returns the packages error instead.
Result<std::filesystem::path> Install(const config::Config& config, const Emulator& emulator);

struct FirmwareState {
  const Firmware* firmware;
  bool present = false;
};
std::vector<FirmwareState> FirmwareStatus(const Emulator& emulator);
// Puts `files` where the emulator looks: a zip of Switch firmware is unpacked, PS3 firmware installed by RPCS3.
Result<void> AddFirmware(const config::Config& config, const Emulator& emulator, const Firmware& firmware,
                         const std::vector<std::filesystem::path>& files);

// A ROM's name without its extension and dump tags: "Metroid Fusion (USA) [!].gba" reads "Metroid Fusion".
std::string CleanName(std::string_view file_name);
// The game's files: its ROM and those a .cue, .gdi or .m3u names, or its folder.
std::vector<std::filesystem::path> GameFiles(const model::Game& game);

// Copies (or moves) `files` into the emulator's folder with the files they name, then imports it.
Result<library::ImportSummary> AddFiles(config::Config& config, store::GameStore& games, api::EventBus& events,
                                        const Emulator& emulator, const std::vector<std::filesystem::path>& files,
                                        bool move);

// The other installed emulators `game` could run with: sharing a console and taking its file.
std::vector<const Emulator*> Alternatives(const config::Config& config, const model::Game& game);
// Moves the game's files into `to`'s folder; it keeps its id, art, tags and playtime.
Result<model::Game> MoveGame(config::Config& config, store::GameStore& games, const model::Game& game,
                             const Emulator& to);

Result<library::ImportSummary> Import(config::Config& config, store::GameStore& games, api::EventBus& events,
                                      const Emulator& emulator);
// Imports from every added emulator source; used by scans.
library::ImportSummary ImportAll(config::Config& config, store::GameStore& games, api::EventBus& events);

}  // namespace mira::emulators
