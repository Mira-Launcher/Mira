#include "emulators/Emulators.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/Relocate.h"
#include "runner/Curl.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "system/Packages.h"

namespace mira::emulators {
namespace {
namespace fs = std::filesystem;

std::string GitHub(std::string_view repo) {
  return std::format("https://api.github.com/repos/{}/releases/latest", repo);
}

}  // namespace

const std::vector<Emulator>& All() {
  static const std::vector<Emulator> emulators = {
      {.id = "duckstation", .name = "DuckStation", .consoles = {"ps1"},
       .extensions = {".cue", ".chd", ".m3u", ".pbp", ".iso", ".img", ".bin", ".ecm", ".mds"},
       .args = {"-batch", "--", "{rom}"}, .fullscreen = {"-fullscreen"},
       .release_url = GitHub("stenzek/duckstation"), .asset = "DuckStation-x64.AppImage",
       .binary = "duckstation.AppImage",
       .firmware = {{.id = "bios", .label = "PlayStation BIOS", .dir = "$XDG_CONFIG_HOME/duckstation/bios|~/.local/share/duckstation/bios",
                     .patterns = {"*.bin", "*.BIN"}}},
       .prefill = {{"$XDG_CONFIG_HOME/duckstation/settings.ini|~/.local/share/duckstation/settings.ini",
                    "[Main]\nSetupWizardIncomplete = false\nNoDesktopFile = true\n"
                    "[AutoUpdater]\nCheckAtStartup = false\n"}}},
      {.id = "pcsx2", .name = "PCSX2", .consoles = {"ps2"},
       .extensions = {".iso", ".chd", ".cso", ".zso", ".gz", ".bin", ".cue", ".m3u", ".mdf", ".elf"},
       .args = {"-batch", "--", "{rom}"}, .fullscreen = {"-fullscreen"},
       .release_url = GitHub("PCSX2/pcsx2"), .asset = "pcsx2-*-linux-appimage-x64-Qt.AppImage",
       .binary = "pcsx2.AppImage",
       .firmware = {{.id = "bios", .label = "PlayStation 2 BIOS", .dir = "~/.config/PCSX2/bios",
                     .patterns = {"*.bin", "*.BIN"}}},
       .prefill = {{"~/.config/PCSX2/inis/PCSX2.ini",
                    "[UI]\nSettingsVersion = 1\nSetupWizardIncomplete = false\n[AutoUpdater]\nCheckAtStartup = false\n"}}},
      {.id = "ppsspp", .name = "PPSSPP", .consoles = {"psp"},
       .extensions = {".iso", ".cso", ".chd", ".pbp", ".elf", ".prx"},
       .fullscreen = {"--fullscreen"},
       .release_url = GitHub("hrydgard/ppsspp"), .asset = "PPSSPP-*-anylinux-x86_64.AppImage",
       .binary = "ppsspp.AppImage"},
      {.id = "rpcs3", .name = "RPCS3", .consoles = {"ps3"},
       .extensions = {".elf", ".self"},
       .folder_game = "PS3_GAME/USRDIR/EBOOT.BIN",
       .args = {"--no-gui", "{rom}"},
       .release_url = GitHub("RPCS3/rpcs3-binaries-linux"), .asset = "rpcs3-*_linux64.AppImage",
       .binary = "rpcs3.AppImage",
       .firmware = {{.id = "firmware", .label = "PlayStation 3 firmware (PS3UPDAT.PUP)",
                     .dir = "~/.config/rpcs3/dev_flash/vsh/module", .patterns = {"vsh.self"},
                     .install_flag = "--installfw"}},
       .prefill = {{"~/.config/rpcs3/GuiConfigs/CurrentSettings.ini", "[main_window]\ninfoBoxEnabledWelcome=false\n"}}},
      {.id = "shadps4", .name = "shadPS4", .consoles = {"ps4"},
       .folder_game = "eboot.bin",
       .args = {"-g", "{rom}"}, .fullscreen = {"--fullscreen", "true"}, .open_args = {"-b"},
       .release_url = GitHub("shadps4-emu/shadPS4"), .asset = "shadps4-linux-sdl-*.zip",
       .binary = "Shadps4-sdl.AppImage"},
      {.id = "azahar", .name = "Azahar", .consoles = {"3ds"},
       .extensions = {".3ds", ".cci", ".cxi", ".3dsx", ".app", ".elf", ".axf"},
       .fullscreen = {"-f"},
       .release_url = GitHub("azahar-emu/azahar"), .asset = "azahar.AppImage",
       .binary = "azahar.AppImage",
       .firmware = {{.id = "aes_keys", .label = "AES keys, for encrypted games",
                     .dir = "~/.local/share/azahar-emu/sysdata", .patterns = {"aes_keys.txt"}, .required = false}}},
      {.id = "eden", .name = "Eden", .consoles = {"switch"},
       .extensions = {".nsp", ".xci", ".nca", ".nro", ".nso"},
       .args = {"-g", "{rom}"}, .fullscreen = {"-f"},
       .release_url = "https://git.eden-emu.dev/api/v1/repos/eden-emu/eden/releases/latest",
       .asset = "Eden-Linux-v*-amd64-gcc-standard.AppImage",
       .binary = "eden.AppImage",
       .firmware = {{.id = "keys", .label = "Keys (prod.keys)", .dir = "~/.local/share/eden/keys",
                     .patterns = {"prod.keys"}},
                    {.id = "firmware", .label = "Firmware (a zip of .nca files)",
                     .dir = "~/.local/share/eden/nand/system/Contents/registered", .patterns = {"*.nca"}}}},
      {.id = "ryujinx", .name = "Ryujinx", .consoles = {"switch"},
       .extensions = {".nsp", ".xci", ".nca", ".nro", ".nso"},
       .fullscreen = {"--fullscreen"},
       .listing_url = "https://mirror.cachyos.org/repo/x86_64/cachyos/", .asset = "ryujinx-*-x86_64.pkg.tar.zst",
       .binary = "Ryujinx",
       .firmware = {{.id = "keys", .label = "Keys (prod.keys)", .dir = "~/.config/Ryujinx/system",
                     .patterns = {"prod.keys"}}}},
      {.id = "cemu", .name = "Cemu", .consoles = {"wiiu"},
       .extensions = {".wud", ".wux", ".wua", ".rpx", ".iso"},
       .args = {"-g", "{rom}"}, .fullscreen = {"-f"},
       .release_url = GitHub("cemu-project/Cemu"), .asset = "Cemu-*-x86_64.AppImage",
       .binary = "cemu.AppImage",
       .firmware = {{.id = "keys", .label = "Disc keys (keys.txt), for .wud and .wux", .dir = "~/.local/share/Cemu",
                     .patterns = {"keys.txt"}, .required = false}},
       .prefill = {{"~/.config/Cemu/settings.xml",
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<content>\n</content>\n"}}},
      {.id = "xemu", .name = "xemu", .consoles = {"xbox"},
       .extensions = {".iso", ".xiso"},
       .args = {"-dvd_path", "{rom}"}, .fullscreen = {"-full-screen"},
       .release_url = GitHub("xemu-project/xemu"), .asset = "xemu-*-x86_64.AppImage", .exclude = {"-dbg-"},
       .binary = "xemu.AppImage"},
      {.id = "flycast", .name = "Flycast", .consoles = {"dreamcast"},
       .extensions = {".gdi", ".cdi", ".chd", ".cue", ".bin"},
       .release_url = GitHub("flyinghead/flycast"), .asset = "flycast-x86_64-*.AppImage",
       .binary = "flycast.AppImage",
       .firmware = {{.id = "bios", .label = "Dreamcast BIOS (dc_boot.bin)", .dir = "~/.local/share/flycast/data",
                     .patterns = {"dc_boot.bin"}, .required = false}}},
      {.id = "melonds", .name = "melonDS", .consoles = {"nds"},
       .extensions = {".nds", ".dsi", ".ids", ".srl", ".zip"},
       .fullscreen = {"-f"},
       .release_url = GitHub("melonDS-emu/melonDS"), .asset = "melonDS-*-appimage-x86_64.zip",
       .binary = "melonDS-x86_64.AppImage"},
      {.id = "dolphin", .name = "Dolphin", .consoles = {"gamecube", "wii"},
       .extensions = {".iso", ".gcm", ".gcz", ".rvz", ".wbfs", ".wia", ".ciso", ".dol", ".elf", ".wad", ".tgc"},
       .args = {"-b", "-e", "{rom}"}, .fullscreen = {"-C", "Dolphin.Display.Fullscreen=True"},
       .stop_timeout_s = 2, .binary = "dolphin-emu",
       .prefill = {{"~/.config/dolphin-emu/Dolphin.ini", "[Analytics]\nPermissionAsked = True\nEnabled = False\n"}}},
      {.id = "mesen", .name = "Mesen", .consoles = {"nes", "snes", "gb", "gbc", "pce", "sms", "gg", "ws"},
       .extensions = {".nes", ".fds", ".unf", ".sfc", ".smc", ".bs", ".gb", ".gbc", ".pce", ".sms", ".gg", ".ws",
                      ".wsc", ".zip"},
       .fullscreen = {"--fullscreen"},
       .release_url = GitHub("SourMesen/Mesen2"), .asset = "Mesen_*_Linux_x64.zip",
       .binary = "Mesen",
       // Its first run sets up default controls; without ICU its core no longer aborts loading a game.
       .prefill = {{"~/.config/Mesen2/settings.json", "{\"ConfigUpgrade\": 1}\n"}},
       .env = {{"DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1"}}},
      {.id = "snes9x", .name = "Snes9x", .consoles = {"snes"},
       .extensions = {".sfc", ".smc", ".fig", ".swc", ".bs", ".st", ".zip"},
       .binary = "snes9x-gtk"},
      {.id = "bsnes", .name = "bsnes", .consoles = {"snes"},
       .extensions = {".sfc", ".smc", ".bs", ".st", ".zip"},
       .fullscreen = {"--fullscreen"},
       .release_url = "https://api.github.com/repos/bsnes-emu/bsnes/releases/tags/nightly",
       .asset = "bsnes-ubuntu.zip",
       .binary = "bsnes"},
      {.id = "ares", .name = "ares",
       .consoles = {"nes", "snes", "n64", "genesis", "32x", "sms", "gg", "sg1000", "gb", "gbc", "gba", "pce", "ngp",
                    "ws", "colecovision", "atari2600"},
       .extensions = {".nes", ".fds", ".sfc", ".smc", ".n64", ".z64", ".v64", ".md", ".gen", ".smd", ".32x", ".sms",
                      ".gg", ".sg", ".gb", ".gbc", ".gba", ".pce", ".sgx", ".ngp", ".ngc", ".ws", ".wsc", ".pc2", ".col",
                      ".a26", ".zip"},
       .args = {"--no-file-prompt", "{rom}"}, .fullscreen = {"--fullscreen"},
       .binary = "ares"},
      {.id = "blastem", .name = "BlastEm", .consoles = {"genesis"},
       .extensions = {".md", ".gen", ".smd", ".bin", ".zip"},
       .fullscreen = {"-f"},
       .download_url = "https://www.retrodev.com/blastem/blastem64-0.6.2.tar.gz",
       .binary = "blastem"},
      {.id = "mgba", .name = "mGBA", .consoles = {"gb", "gbc", "gba"},
       .extensions = {".gba", ".gb", ".gbc", ".sgb", ".agb", ".zip", ".7z"},
       .fullscreen = {"-f"},
       .release_url = GitHub("mgba-emu/mgba"), .asset = "mGBA-*-appimage-x64.appimage",
       .binary = "mgba.AppImage"},
      {.id = "rmg", .name = "RMG", .consoles = {"n64"},
       .extensions = {".z64", ".n64", ".v64", ".zip", ".7z"},
       .args = {"-q", "{rom}"}, .fullscreen = {"-f"},
       .release_url = GitHub("Rosalie241/RMG"), .asset = "RMG-Portable-Linux64-*.AppImage",
       .binary = "RMG.AppImage"},
      {.id = "vita3k", .name = "Vita3K", .consoles = {"psvita"},
       .extensions = {".vpk"},
       .fullscreen = {"-F"},
       .release_url = GitHub("Vita3K/Vita3K"), .asset = "Vita3K-x86_64.AppImage",
       .binary = "Vita3K.AppImage",
       .firmware = {{.id = "firmware", .label = "PS Vita firmware (PSVUPDAT.PUP)",
                     .dir = "~/.local/share/Vita3K/Vita3K/vs0/sys/external", .patterns = {"*.suprx"},
                     .install_flag = "--firmware"}}},
      {.id = "ymir", .name = "Ymir", .consoles = {"saturn"},
       .extensions = {".cue", ".chd", ".ccd", ".iso", ".mds"},
       .args = {"-u", "{rom}"}, .fullscreen = {"-f"}, .open_args = {"-u"},
       .release_url = "https://api.github.com/repos/StrikerX3/Ymir/releases/tags/latest-nightly",
       .asset = "ymir-linux-x86_64-SSE2-*.tar.xz",
       .binary = "ymir-sdl3",
       .firmware = {{.id = "bios", .label = "Saturn BIOS", .dir = "~/.local/share/StrikerX3/Ymir/roms/ipl",
                     .patterns = {"*.bin", "*.rom"}}}},
      {.id = "kyty", .name = "KytyPS5", .consoles = {"ps5"},
       .extensions = {".elf", ".zar"}, .folder_game = "eboot.bin",
       .args = {"--no-splash", "--game", "{rom}"}, .fullscreen = {"--fullscreen"}, .open_binary = "launcher",
       .release_url = GitHub("KytyPS5/KytyPS5"), .asset = "KytyPS5-*-Linux-x86_64.tar.gz",
       .binary = "kyty_emulator"},
      {.id = "xenia", .name = "Xenia Canary", .consoles = {"xbox360"},
       .extensions = {".iso", ".xex", ".zar"},
       .fullscreen = {"--fullscreen=true"},
       .release_url = GitHub("xenia-canary/xenia-canary-releases"), .asset = "xenia_canary_linux.tar_.xz",
       .binary = "xenia_canary"},
      {.id = "dosbox", .name = "DOSBox Staging", .consoles = {"dos"},
       .extensions = {".exe", ".com", ".bat"}, .folders = true,
       .fullscreen = {"--fullscreen"},
       .release_url = GitHub("dosbox-staging/dosbox-staging"), .asset = "dosbox-staging-linux-x86_64-*.tar.xz",
       .binary = "dosbox"},
      {.id = "scummvm", .name = "ScummVM", .consoles = {"adventure"},
       .folders = true,
       .args = {"-p", "{dir}", "--auto-detect"}, .fullscreen = {"-f"},
       .binary = "scummvm"},
      {.id = "mednafen", .name = "Mednafen", .consoles = {"saturn", "ps1", "pce", "vb", "ngp", "ws", "lynx"},
       .extensions = {".vb", ".vboy", ".ngp", ".ngc", ".ws", ".wsc", ".lnx", ".pce", ".cue", ".chd", ".toc", ".m3u",
                      ".zip"},
       .fullscreen = {"-video.fs", "1"},
       .binary = "mednafen",
       .firmware = {{.id = "bios", .label = "BIOS, for Saturn, PlayStation, PC Engine CD and Lynx games",
                     .dir = "~/.mednafen/firmware",
                     .patterns = {"sega_101.bin", "mpr-17933.bin", "scph5500.bin", "scph5501.bin", "scph5502.bin",
                                  "syscard3.pce", "lynxboot.img"},
                     .required = false}}},
      {.id = "stella", .name = "Stella", .consoles = {"atari2600"},
       .extensions = {".a26", ".bin", ".zip"},
       .fullscreen = {"-fullscreen", "1"},
       .binary = "stella"},
      {.id = "mupen64plus", .name = "Mupen64Plus", .consoles = {"n64"},
       .extensions = {".n64", ".z64", ".v64"},
       .fullscreen = {"--fullscreen"},
       .binary = "mupen64plus"},
      {.id = "redream", .name = "Redream", .consoles = {"dreamcast"},
       .extensions = {".gdi", ".cdi", ".chd", ".cue"},
       .download_url = "https://redream.io/download/redream.x86_64-linux-v1.5.0.tar.gz",
       .binary = "redream"},
      {.id = "atari800", .name = "atari800", .consoles = {"atari8bit", "atari5200"},
       .extensions = {".xex", ".atr", ".atx", ".xfd", ".car", ".cas"},
       .fullscreen = {"-fullscreen"},
       .release_url = GitHub("atari800/atari800"), .asset = "Atari800-*-x86_64.AppImage",
       .binary = "atari800.AppImage"},
      {.id = "hatari", .name = "Hatari", .consoles = {"atarist"},
       .extensions = {".st", ".msa", ".stx", ".dim"},
       .fullscreen = {"--fullscreen"},
       .binary = "hatari"},
      {.id = "fsuae", .name = "FS-UAE", .consoles = {"amiga"},
       .extensions = {".adf", ".adz", ".dms", ".ipf"},
       .fullscreen = {"--fullscreen"},
       .release_url = GitHub("FrodeSolheim/fs-uae"), .asset = "FS-UAE_*_Linux_x86-64.tar.xz",
       .binary = "fs-uae"},
      {.id = "vice", .name = "VICE", .consoles = {"c64"},
       .extensions = {".d64", ".g64", ".t64", ".prg", ".p00", ".crt", ".tap"},
       .fullscreen = {"-fullscreen"},
       .binary = "x64sc"},
      {.id = "openmsx", .name = "openMSX", .consoles = {"msx"},
       .extensions = {".rom", ".mx1", ".mx2", ".dsk", ".cas"},
       .fullscreen = {"-command", "set fullscreen on"},
       .release_url = GitHub("openMSX/openMSX"), .asset = "openmsx-*-linux-x86_64-bin.zip",
       .binary = "openmsx"},
      {.id = "uzdoom", .name = "UZDoom", .consoles = {"doom"},
       .extensions = {".wad", ".iwad", ".ipk3"},
       .args = {"-iwad", "{rom}"}, .fullscreen = {"+vid_fullscreen", "1"},
       .release_url = GitHub("UZDoom/UZDoom"), .asset = "Linux-UZDoom-Release-x86_64.AppImage",
       .binary = "UZDoom.AppImage"},
      {.id = "easyrpg", .name = "EasyRPG Player", .consoles = {"rpgmaker"},
       .folder_game = "RPG_RT.ldb",
       .args = {"--project-path", "{dir}"}, .fullscreen = {"--fullscreen"},
       .download_url = "https://easyrpg.org/downloads/player/0.8.1.1/easyrpg-player-0.8.1.1-linux.tar.gz",
       .binary = "easyrpg-player"},
      // Arcade sets keep their zip's name, which MAME looks up in the folder.
      {.id = "mame", .name = "MAME", .consoles = {"arcade"},
       .extensions = {".zip", ".7z", ".chd"},
       .args = {"-skip_gameinfo", "-rompath", "{dir}", "{stem}"},
       .binary = "mame"},
  };
  return emulators;
}

const Emulator* Find(std::string_view id) {
  const auto& all = All();
  const auto it = std::ranges::find(all, id, &Emulator::id);
  return it == all.end() ? nullptr : &*it;
}

const Emulator* ForGame(const model::Game& game) { return Find(game.source); }

bool Downloads(const Emulator& emulator) {
  return !emulator.release_url.empty() || !emulator.listing_url.empty() || !emulator.download_url.empty();
}

namespace {

fs::path ToolDir(const config::Config& config, const Emulator& emulator) {
  return config.File().parent_path() / "tools" / emulator.id;
}

// "~" is home; "~/.config" and "~/.local/share" follow XDG_CONFIG_HOME and XDG_DATA_HOME, as emulators do.
// "$VAR/a|~/b" is $VAR/a when VAR is set, else ~/b.
fs::path Expand(const std::string& dir) {
  if (dir.starts_with('$') && dir.contains('|')) {
    const std::string first = dir.substr(1, dir.find('|') - 1);
    const char* value = std::getenv(first.substr(0, first.find('/')).c_str());
    if (value && value[0] == '/') return fs::path(value) / first.substr(first.find('/') + 1);
    return Expand(dir.substr(dir.find('|') + 1));
  }
  if (!dir.starts_with("~/")) return dir;
  const std::string rest = dir.substr(2);
  for (const auto& [prefix, variable] : {std::pair{".config", "XDG_CONFIG_HOME"}, {".local/share", "XDG_DATA_HOME"}}) {
    const char* home = std::getenv(variable);
    if (home && home[0] == '/' && (rest == prefix || rest.starts_with(std::string(prefix) + "/")))
      return fs::path(home) / fs::path(rest).lexically_relative(prefix);
  }
  return paths::Home() / dir.substr(2);
}

bool Accepts(const Emulator& emulator, const fs::path& file) {
  return std::ranges::contains(emulator.extensions, strings::ToLower(file.extension().string()));
}

bool IsFolderGame(const Emulator& emulator, const fs::path& dir) {
  std::error_code ec;
  return !emulator.folder_game.empty() && fs::is_regular_file(dir / emulator.folder_game, ec);
}

struct StartCandidates {
  std::vector<std::string> files;  // likeliest first
  bool named = false;              // the first is named like the folder
};

// The files a `folders` emulator could start a game folder with, likeliest first: one named like the folder,
// then those that aren't its setup, each in the order of the emulator's extensions. Any file when it lists
// none (ScummVM finds the game itself).
StartCandidates RankStartFiles(const Emulator& emulator, const fs::path& dir) {
  const auto plain = [](std::string text) {
    std::erase_if(text, [](unsigned char c) { return !std::isalnum(c); });
    return strings::ToLower(text);
  };
  static const std::regex setup(R"(setup|install|uninst|config|setsound|sound)", std::regex::icase);
  const std::string folder = plain(dir.filename().string());
  std::vector<std::pair<int, std::string>> ranked;
  for (const fs::path& file : paths::ListDir(dir)) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) continue;
    const auto listed = std::ranges::find(emulator.extensions, strings::ToLower(file.extension().string()));
    if (!emulator.extensions.empty() && listed == emulator.extensions.end()) continue;
    const std::string stem = file.stem().string();
    int rank = 100 - static_cast<int>(listed - emulator.extensions.begin());
    if (plain(stem) == folder) rank += 1000;
    else if (!std::regex_search(stem, setup)) rank += 500;
    ranked.emplace_back(-rank, file.filename().string());
  }
  std::ranges::stable_sort(ranked, {}, &std::pair<int, std::string>::first);
  StartCandidates out;
  out.named = !ranked.empty() && -ranked.front().first >= 1000;
  for (auto& [rank, name] : ranked) out.files.push_back(std::move(name));
  return out;
}

std::string StartFile(const Emulator& emulator, const fs::path& dir) {
  const StartCandidates candidates = RankStartFiles(emulator, dir);
  return candidates.files.empty() ? std::string() : candidates.files.front();
}

// The files a .cue/.gdi (FILE "x" lines, or a .gdi's third field) or .m3u (one per line) names, beside it.
std::vector<fs::path> Named(const fs::path& file) {
  const std::string ext = strings::ToLower(file.extension().string());
  if (ext != ".cue" && ext != ".gdi" && ext != ".m3u") return {};
  std::vector<fs::path> named;
  std::ifstream in(file);
  std::string line;
  static const std::regex cue_file(R"re(^\s*FILE\s+"([^"]+)")re", std::regex::icase);
  static const std::regex gdi_track(R"re(^\s*\d+\s+\d+\s+\d+\s+\d+\s+"?([^"]+?)"?\s+\d+\s*$)re");
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch match;
    if (ext == ".cue" && std::regex_search(line, match, cue_file)) named.push_back(file.parent_path() / match[1].str());
    if (ext == ".gdi" && std::regex_match(line, match, gdi_track)) named.push_back(file.parent_path() / match[1].str());
    if (ext == ".m3u" && !line.empty() && !line.starts_with('#')) {
      const fs::path disc = file.parent_path() / line;
      named.push_back(disc);
      std::ranges::move(Named(disc), std::back_inserter(named));
    }
  }
  return named;
}

// A ROM and every file it names, to copy, move or delete together.
std::vector<fs::path> WithNamed(const fs::path& rom) {
  std::vector<fs::path> files = {rom};
  for (const fs::path& named : Named(rom)) {
    if (!std::ranges::contains(files, named)) files.push_back(named);
  }
  return files;
}

struct Found {
  fs::path dir;
  std::string exe;  // relative to dir
};

// A homebrew's EBOOT.PBP or boot.elf: named, and added, by its folder.
bool IsGenericName(const fs::path& file) {
  const std::string stem = strings::ToLower(file.stem().string());
  return stem == "eboot" || stem == "boot";
}

// The games in `root`: folder games, or files it takes that no .cue/.gdi/.m3u beside them names.
void FindGames(const Emulator& emulator, const fs::path& root, int depth, std::vector<Found>& found) {
  std::set<fs::path> named;
  std::vector<fs::path> files;
  for (const fs::path& entry : paths::ListDir(root)) {
    if (entry.filename().string().starts_with('.')) continue;
    std::error_code ec;
    if (fs::is_directory(entry, ec)) {
      if (emulator.folders) {
        if (const std::string start = StartFile(emulator, entry); !start.empty()) found.push_back({entry, start});
      } else if (IsFolderGame(emulator, entry)) found.push_back({entry, emulator.folder_game});
      else if (depth < 2) FindGames(emulator, entry, depth + 1, found);
      continue;
    }
    if (!Accepts(emulator, entry)) continue;
    files.push_back(entry);
    for (const fs::path& file : Named(entry)) named.insert(file.lexically_normal());
  }
  // A homebrew's folder is one game, its EBOOT.PBP: the modules beside it (.prx) aren't games.
  if (depth > 0) {
    const auto boot = std::ranges::find_if(files, [](const fs::path& file) { return IsGenericName(file); });
    if (boot != files.end()) return found.push_back({root, boot->filename().string()});
  }
  for (const fs::path& file : files) {
    if (!named.contains(file.lexically_normal())) found.push_back({root, file.filename().string()});
  }
}

std::string Slug(const Emulator& emulator, const fs::path& rom_dir, const Found& item) {
  const fs::path rel = ByFolder(emulator) && item.dir != rom_dir
                           ? item.dir.lexically_relative(rom_dir)
                           : (item.dir / item.exe).lexically_relative(rom_dir).replace_extension();
  return strings::Slugify(rel.string());
}

// Copies or moves `from` to `to`, keeping what's already there.
Result<void> Transfer(const fs::path& from, const fs::path& to, bool move) {
  std::error_code ec;
  if (fs::exists(to, ec)) return {};
  fs::create_directories(to.parent_path(), ec);
  if (move) return library::MovePath(from, to, true);
  fs::copy(from, to, fs::copy_options::recursive, ec);
  if (ec) return Err("copy_failed", std::format("couldn't copy {}: {}", from.filename().string(), ec.message()));
  return {};
}

// The newest file matching `asset` the folder listing at `listing_url` links to, by version.
Result<runner::ReleaseAsset> NewestListed(const config::Config& config, const Emulator& emulator) {
  const fs::path page = ToolDir(config, emulator).parent_path() / (emulator.id + "-listing.html");
  std::error_code ec;
  fs::create_directories(page.parent_path(), ec);
  if (auto fetched = runner::CurlDownload(emulator.listing_url, page); !fetched)
    return std::unexpected(fetched.error());
  std::ifstream in(page);
  const std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  fs::remove(page, ec);
  static const std::regex href(R"re(href="([^"/?]+)")re");
  std::string newest;
  for (auto it = std::sregex_iterator(html.begin(), html.end(), href); it != std::sregex_iterator(); ++it) {
    const std::string name = (*it)[1].str();
    if (strings::GlobMatch(emulator.asset, name) && (newest.empty() || strverscmp(name.c_str(), newest.c_str()) > 0))
      newest = name;
  }
  if (newest.empty())
    return Err("no_release_asset", std::format("{} lists no file matching {}", emulator.listing_url, emulator.asset));
  runner::ReleaseAsset asset;
  asset.tag = newest;
  asset.asset_name = newest;
  asset.download_url = emulator.listing_url + newest;
  return asset;
}

}  // namespace

void Prefill(const Emulator& emulator) {
  for (const auto& [file, content] : emulator.prefill) {
    const fs::path path = Expand(file);
    std::error_code ec;
    if (fs::exists(path, ec)) continue;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path) << content;
  }
}

fs::path RomDir(const config::Config& config, const Emulator& emulator) {
  return config.GetPath("emulators.roms_root") / emulator.name;
}

fs::path Binary(const config::Config& config, const Emulator& emulator) {
  std::error_code ec;
  if (const fs::path chosen = config.GetPath(std::format("emulators.{}.bin", emulator.id)); !chosen.empty())
    return fs::is_regular_file(chosen, ec) ? chosen : fs::path();
  if (Downloads(emulator)) {
    // An archive may keep it in a folder of its own.
    const auto installed = runner::InstalledToolOf(config, emulator.id);
    if (installed && fs::is_regular_file(installed->binary, ec)) return installed->binary;
  }
  if (auto found = runner::FindOnPath(emulator.binary)) return *found;
  return {};
}

std::string InstalledVersion(const config::Config& config, const Emulator& emulator) {
  const auto installed = runner::InstalledToolOf(config, emulator.id);
  return installed ? installed->tag : "";
}

constexpr std::string_view kGithubApi = "https://api.github.com/repos/";

std::string DefaultSource(const Emulator& emulator) {
  if (emulator.release_url.starts_with(kGithubApi)) {
    const std::string rest = emulator.release_url.substr(kGithubApi.size());
    return rest.substr(0, rest.find('/', rest.find('/') + 1));
  }
  if (!emulator.release_url.empty()) return emulator.release_url;
  return emulator.listing_url.empty() ? emulator.download_url : emulator.listing_url;
}

Result<runner::ReleaseAsset> Latest(const config::Config& config, const Emulator& emulator) {
  const std::string chosen = config.GetString(std::format("emulators.{}.repo", emulator.id));
  if (!chosen.empty() && chosen != DefaultSource(emulator)) {
    const std::string repo = strings::GithubRepo(chosen);
    if (repo.starts_with("https://")) return runner::LatestRelease(repo, emulator.asset, emulator.exclude);
    // A fork on GitHub: the same release of it (bsnes's is the nightly tag).
    const std::string default_repo = DefaultSource(emulator);
    std::string url = GitHub(repo);
    if (emulator.release_url.starts_with(kGithubApi))
      url = std::string(kGithubApi) + repo + emulator.release_url.substr(kGithubApi.size() + default_repo.size());
    return runner::LatestRelease(url, emulator.asset, emulator.exclude);
  }
  if (!emulator.release_url.empty()) return runner::LatestRelease(emulator.release_url, emulator.asset, emulator.exclude);
  if (!emulator.listing_url.empty()) return NewestListed(config, emulator);
  // One fixed file, named by its version.
  runner::ReleaseAsset asset;
  asset.asset_name = fs::path(emulator.download_url).filename().string();
  asset.download_url = emulator.download_url;
  asset.tag = asset.asset_name;
  return asset;
}

Result<fs::path> Install(const config::Config& config, const Emulator& emulator) {
  if (!Downloads(emulator)) {
    if (auto ready = system::RequireFeature(emulator.id, emulator.name); !ready) return std::unexpected(ready.error());
    return Binary(config, emulator);
  }
  static std::mutex mutex;
  static std::set<std::string> installing;
  {
    std::lock_guard lock(mutex);
    if (!installing.insert(emulator.id).second)
      return Err("already_installing", std::format("{} is already being installed", emulator.name));
  }
  struct Done {
    const std::string& id;
    ~Done() {
      std::lock_guard lock(mutex);
      installing.erase(id);
    }
  } done{emulator.id};
  const Result<runner::ReleaseAsset> asset = Latest(config, emulator);
  if (!asset) return std::unexpected(asset.error());
  auto installed = runner::InstallToolBinary(config, emulator.id, *asset, emulator.binary);
  if (!installed) return installed;
  // Libraries the download doesn't carry (bsnes's libao).
  if (auto ready = system::RequireFeature(emulator.id, emulator.name); !ready) return std::unexpected(ready.error());
  return installed;
}

std::vector<FirmwareState> FirmwareStatus(const Emulator& emulator) {
  std::vector<FirmwareState> states;
  for (const Firmware& firmware : emulator.firmware) {
    const auto entries = paths::ListDir(Expand(firmware.dir));
    const bool present = std::ranges::any_of(entries, [&](const fs::path& entry) {
      return std::ranges::any_of(firmware.patterns, [&](const std::string& pattern) {
        return strings::GlobMatch(pattern, entry.filename().string());
      });
    });
    states.push_back({&firmware, present});
  }
  return states;
}

Result<void> AddFirmware(const config::Config& config, const Emulator& emulator, const Firmware& firmware,
                         const std::vector<fs::path>& files) {
  const fs::path dir = Expand(firmware.dir);
  std::error_code ec;
  for (const fs::path& file : files) {
    const std::string ext = strings::ToLower(file.extension().string());
    if (!firmware.install_flag.empty()) {
      const fs::path binary = Binary(config, emulator);
      if (binary.empty())
        return Err("emulator_missing", std::format("Install {} first, then add its firmware.", emulator.name));
      Command command;
      command.argv = {binary.string(), firmware.install_flag, file.string()};
      const Result<runner::ExecResult> ran = runner::RunAndWait(command);
      // RPCS3 exits non-zero once its done dialog is closed, so its files have the last word.
      const auto states = FirmwareStatus(emulator);
      const bool present = std::ranges::any_of(
          states, [&](const FirmwareState& state) { return state.firmware == &firmware && state.present; });
      if (!ran || (ran->exit_code != 0 && !present))
        return Err("firmware_failed", std::format("{} couldn't install that firmware file.", emulator.name));
      continue;
    }
    fs::create_directories(dir, ec);
    if (ext == ".zip") {
      if (auto extracted = runner::Extract(file, dir); !extracted) return extracted;
      continue;
    }
    fs::copy_file(file, dir / file.filename(), fs::copy_options::overwrite_existing, ec);
    if (ec) return Err("copy_failed", std::format("couldn't copy {}: {}", file.filename().string(), ec.message()));
  }
  return {};
}

std::string CleanName(std::string_view file_name) {
  std::string name(file_name);
  if (const auto dot = name.rfind('.'); dot != std::string::npos && dot > 0) name.resize(dot);
  // Disc numbers tell discs apart; every other () or [] group is region, revision or dump info.
  static const std::regex tag(R"(\s*[\(\[]([^\)\]]*)[\)\]])");
  std::string out;
  auto begin = std::sregex_iterator(name.begin(), name.end(), tag);
  std::size_t last = 0;
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    out += name.substr(last, it->position() - last);
    if (strings::ToLower((*it)[1].str()).starts_with("disc")) out += " (" + (*it)[1].str() + ")";
    last = it->position() + it->length();
  }
  out += name.substr(last);
  std::ranges::replace(out, '_', ' ');
  return strings::Trim(out);
}

bool ByFolder(const Emulator& emulator) { return !emulator.folder_game.empty() || emulator.folders; }

fs::path MainFile(const model::Game& game) { return fs::path(game.install_path) / game.exe_path; }

bool IsLooseFile(const config::Config& config, const model::Game& game) {
  const Emulator* emulator = ForGame(game);
  return emulator != nullptr && fs::path(game.install_path) == RomDir(config, *emulator);
}

std::vector<std::string> StartChoices(const config::Config& config, const model::Game& game) {
  const Emulator* emulator = ForGame(game);
  if (emulator == nullptr || !emulator->folders || emulator->extensions.empty() || IsLooseFile(config, game)) return {};
  StartCandidates candidates = RankStartFiles(*emulator, game.install_path);
  if (candidates.named || candidates.files.size() < 2) return {};
  return std::move(candidates.files);
}

std::vector<fs::path> GameFiles(const config::Config& config, const model::Game& game) {
  const Emulator* emulator = ForGame(game);
  if (emulator != nullptr && ByFolder(*emulator) && !IsLooseFile(config, game)) return {game.install_path};
  return WithNamed(MainFile(game));
}

Result<library::ImportSummary> AddFiles(config::Config& config, store::GameStore& games, api::EventBus& events,
                                        const Emulator& emulator, const std::vector<fs::path>& files, bool move) {
  const fs::path dir = RomDir(config, emulator);
  std::vector<std::string> refused;
  for (const fs::path& file : files) {
    std::error_code ec;
    // A homebrew comes as its folder: picked itself, or by its EBOOT.PBP.
    const bool generic = Accepts(emulator, file) && IsGenericName(file);
    const fs::path source = generic ? file.parent_path() : file;
    const bool folder = fs::is_directory(source, ec);
    const auto holds_game = [&] {
      if (emulator.folders) return !StartFile(emulator, source).empty();
      return std::ranges::any_of(paths::ListDir(source), [&](const fs::path& entry) { return Accepts(emulator, entry); });
    };
    if (folder ? !IsFolderGame(emulator, source) && !holds_game() : !Accepts(emulator, file)) {
      refused.push_back(file.filename().string());
      continue;
    }
    if (folder) {
      if (auto done = Transfer(source, dir / source.filename(), move); !done) return std::unexpected(done.error());
      continue;
    }
    for (const fs::path& part : WithNamed(file)) {
      if (auto done = Transfer(part, dir / part.lexically_relative(file.parent_path()), move); !done)
        return std::unexpected(done.error());
    }
  }
  if (!refused.empty() && refused.size() == files.size()) {
    std::string names;
    for (const std::string& name : refused) names += (names.empty() ? "" : ", ") + name;
    return Err("wrong_files", std::format("{} doesn't run {}", emulator.name, names));
  }
  return Import(config, games, events, emulator);
}

std::map<std::string, std::string> Environment(const Emulator& emulator) {
  // Mira updates the emulators it downloads; some AppImages would otherwise ask to update themselves.
  std::map<std::string, std::string> env = {{"DISABLE_AUTO_UPDATES", "1"}};
  for (const auto& [key, value] : emulator.env) env[key] = value;
  return env;
}

const Emulator* Recognize(const fs::path& appimage) {
  const std::string file = strings::ToLower(appimage.filename().string());
  for (const Emulator& emulator : All()) {
    if (!emulator.asset.empty() && strings::GlobMatch(strings::ToLower(emulator.asset), file)) return &emulator;
    // Another build of it: its name, then a separator and the build's platform. A version alone isn't enough:
    // "ares-of-war-1.0" is a game.
    for (std::string name : {emulator.id, strings::ToLower(emulator.name)}) {
      std::erase(name, ' ');
      if (!file.starts_with(name) || file.size() <= name.size() + 1) continue;
      const char separator = file[name.size()];
      const std::string rest = file.substr(name.size() + 1);
      const bool build = rest.contains("x86") || rest.contains("x64") || rest.contains("amd64") ||
                         rest.contains("linux") || rest == "appimage";
      if ((separator == '-' || separator == '_' || separator == '.') && build) return &emulator;
    }
  }
  return nullptr;
}

void Adopt(config::Config& config, store::GameStore& games, api::EventBus& events, const Emulator& emulator,
           const fs::path& appimage) {
  bool changed = false;
  if (Binary(config, emulator).empty()) {
    if (auto set = config.Set(std::format("emulators.{}.bin", emulator.id), appimage.string()); !set)
      log::Warn("couldn't use {} for {}: {}", appimage.string(), emulator.name, set.error().message);
    changed = true;
  }
  if (!games.Source(emulator.id).added) {
    games.MarkSourceAdded(emulator.id, false);
    changed = true;
  }
  if (!changed) return;
  events.Publish("sources.changed", nlohmann::json::object());
  log::Info("{} is {}, added as its source", appimage.string(), emulator.name);
}

std::vector<const Emulator*> Alternatives(const config::Config& config, const model::Game& game) {
  const Emulator* from = ForGame(game);
  std::vector<const Emulator*> out;
  if (from == nullptr) return out;
  for (const Emulator& emulator : All()) {
    if (&emulator == from || Binary(config, emulator).empty()) continue;
    const bool shares = std::ranges::any_of(from->consoles, [&](const std::string& console) {
      return std::ranges::contains(emulator.consoles, console);
    });
    const bool takes = ByFolder(*from) ? emulator.folder_game == from->folder_game && emulator.folders == from->folders
                                       : Accepts(emulator, game.exe_path);
    if (shares && takes) out.push_back(&emulator);
  }
  return out;
}

Result<model::Game> MoveGame(config::Config& config, store::GameStore& games, const model::Game& game,
                             const Emulator& to) {
  const Emulator* from = ForGame(game);
  if (from == nullptr) return Err("not_emulator_game", std::format("{} isn't an emulator's game", game.name));
  const fs::path from_dir = RomDir(config, *from);
  const fs::path to_dir = RomDir(config, to);
  for (const fs::path& file : GameFiles(config, game)) {
    const fs::path target = to_dir / file.lexically_relative(from_dir);
    std::error_code ec;
    if (fs::exists(target, ec)) return Err("already_there", std::format("{} already has {}", to.name, game.name));
    fs::create_directories(target.parent_path(), ec);
    if (auto moved = library::MovePath(file, target, true); !moved) return std::unexpected(moved.error());
  }
  const fs::path install = to_dir / fs::path(game.install_path).lexically_relative(from_dir);
  return games.Update(game.id, [&](model::Game& g) {
    g.source = to.id;
    g.runner_ref = "emulator:" + to.id;
    g.install_path = install.lexically_normal().string();
  });
}

Result<library::ImportSummary> Import(config::Config& config, store::GameStore& games, api::EventBus& events,
                                      const Emulator& emulator) {
  const fs::path dir = RomDir(config, emulator);
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::vector<Found> found;
  FindGames(emulator, dir, 0, found);

  // Known games by their file (or their folder, whose start file the user may have changed), so a game moved
  // here from another emulator keeps its id.
  const auto key = [&](const fs::path& folder, const std::string& file) {
    return (emulator.folders && folder != dir ? folder : folder / file).lexically_normal();
  };
  std::map<fs::path, model::Game> by_file;
  for (const model::Game& game : games.All()) {
    if (game.source == emulator.id) by_file.emplace(key(game.install_path, game.exe_path), game);
  }

  library::ImportSummary summary;
  std::set<fs::path> found_keys;
  for (const Found& item : found) found_keys.insert(key(item.dir, item.exe));
  for (const Found& item : found) {
    std::optional<model::Game> existing;
    if (const auto known = by_file.find(key(item.dir, item.exe)); known != by_file.end()) existing = known->second;
    model::Game game = existing.value_or(model::Game{});
    if (!existing) {
      const std::string base = std::format("{}-{}", emulator.id, Slug(emulator, dir, item));
      game.id = base;
      for (int n = 2; games.Find(game.id); ++n) game.id = std::format("{}-{}", base, n);
      const bool generic = IsGenericName(item.exe) && item.dir != dir;
      game.name = CleanName((ByFolder(emulator) && item.dir != dir) || generic ? item.dir.filename().string() : item.exe);
    }
    game.source = emulator.id;
    game.install_path = item.dir.string();
    // A start file the user chose stays while it's there.
    if (!existing || !fs::is_regular_file(item.dir / existing->exe_path, ec)) game.exe_path = item.exe;
    game.platform = model::Platform::Native;
    game.runner_ref = "emulator:" + emulator.id;
    game.status = model::GameStatus::Ready;
    game.last_error.clear();
    if (existing && model::ToJson(game) == model::ToJson(*existing)) continue;
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;
    if (auto saved = games.Merge(existing, game); !saved) {
      log::Error("failed to import {} game {}: {}", emulator.name, game.id, saved.error().message);
      continue;
    }
    library::RecordImported(summary, events, game, existing.has_value());
  }

  for (const model::Game& game : games.All()) {
    if (game.source != emulator.id || game.status == model::GameStatus::Missing) continue;
    // Still there but no longer a game of its own (a homebrew's module, once its folder became one game).
    const bool exists = fs::exists(MainFile(game), ec);
    if (exists && found_keys.contains(key(game.install_path, game.exe_path))) continue;
    if (exists || config.GetBool("library.remove_missing")) {
      if (games.Remove(game.id)) events.Publish("game.removed", {{"id", game.id}});
      continue;
    }
    if (auto missing = games.Update(game.id, [](model::Game& g) { g.status = model::GameStatus::Missing; })) {
      events.Publish("game.updated", model::ToJson(*missing));
    }
  }
  return summary;
}

library::ImportSummary ImportAll(config::Config& config, store::GameStore& games, api::EventBus& events) {
  library::ImportSummary total;
  for (const Emulator& emulator : All()) {
    const store::SourceState state = games.Source(emulator.id);
    if (!state.added || !state.enabled) continue;
    const auto summary = Import(config, games, events, emulator);
    if (!summary) continue;
    total.added += summary->added;
    total.updated += summary->updated;
    std::ranges::move(summary->added_games, std::back_inserter(total.added_games));
  }
  return total;
}

}  // namespace mira::emulators
