#include "emulators/Emulators.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/Relocate.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "system/Packages.h"

namespace mira::emulators {
namespace {
namespace fs = std::filesystem;

std::string GitHub(std::string_view repo) { return std::format("https://api.github.com/repos/{}/releases/latest", repo); }

}  // namespace

const std::vector<Emulator>& All() {
  static const std::vector<Emulator> emulators = {
      {.id = "duckstation", .name = "DuckStation", .consoles = {"ps1"},
       .extensions = {".cue", ".chd", ".m3u", ".pbp", ".iso", ".img", ".bin", ".ecm", ".mds"},
       .args = {"-batch", "--", "{rom}"},
       .release_url = GitHub("stenzek/duckstation"), .asset = "DuckStation-x64.AppImage",
       .binary = "duckstation.AppImage",
       .firmware = {{.id = "bios", .label = "PlayStation BIOS", .dir = "~/.local/share/duckstation/bios",
                     .patterns = {"*.bin", "*.BIN"}}}},
      {.id = "pcsx2", .name = "PCSX2", .consoles = {"ps2"},
       .extensions = {".iso", ".chd", ".cso", ".zso", ".gz", ".bin", ".cue", ".m3u", ".mdf", ".elf"},
       .args = {"-batch", "--", "{rom}"},
       .release_url = GitHub("PCSX2/pcsx2"), .asset = "pcsx2-*-linux-appimage-x64-Qt.AppImage",
       .binary = "pcsx2.AppImage",
       .firmware = {{.id = "bios", .label = "PlayStation 2 BIOS", .dir = "~/.config/PCSX2/bios",
                     .patterns = {"*.bin", "*.BIN"}}}},
      {.id = "ppsspp", .name = "PPSSPP", .consoles = {"psp"},
       .extensions = {".iso", ".cso", ".chd", ".pbp", ".elf", ".prx"},
       .args = {"{rom}"},
       .release_url = GitHub("hrydgard/ppsspp"), .asset = "PPSSPP-*-anylinux-x86_64.AppImage",
       .binary = "ppsspp.AppImage"},
      {.id = "rpcs3", .name = "RPCS3", .consoles = {"ps3"},
       .folder_game = "PS3_GAME/USRDIR/EBOOT.BIN",
       .args = {"--no-gui", "{rom}"},
       .release_url = GitHub("RPCS3/rpcs3-binaries-linux"), .asset = "rpcs3-*_linux64.AppImage",
       .binary = "rpcs3.AppImage",
       .firmware = {{.id = "firmware", .label = "PlayStation 3 firmware (PS3UPDAT.PUP)",
                     .dir = "~/.config/rpcs3/dev_flash/vsh/module", .patterns = {"vsh.self"}}}},
      {.id = "shadps4", .name = "shadPS4", .consoles = {"ps4"},
       .folder_game = "eboot.bin",
       .args = {"-g", "{rom}"},
       .release_url = GitHub("shadps4-emu/shadPS4"), .asset = "shadps4-linux-sdl-*.zip",
       .binary = "Shadps4-sdl.AppImage"},
      {.id = "azahar", .name = "Azahar", .consoles = {"3ds"},
       .extensions = {".3ds", ".cci", ".cxi", ".3dsx", ".app", ".elf", ".axf"},
       .args = {"{rom}"},
       .release_url = GitHub("azahar-emu/azahar"), .asset = "azahar.AppImage",
       .binary = "azahar.AppImage",
       .firmware = {{.id = "aes_keys", .label = "AES keys, for encrypted games",
                     .dir = "~/.local/share/azahar-emu/sysdata", .patterns = {"aes_keys.txt"}, .required = false}}},
      {.id = "eden", .name = "Eden", .consoles = {"switch"},
       .extensions = {".nsp", ".xci", ".nca", ".nro", ".nso"},
       .args = {"-g", "{rom}"},
       .release_url = "https://git.eden-emu.dev/api/v1/repos/eden-emu/eden/releases/latest",
       .asset = "Eden-Linux-v*-amd64-gcc-standard.AppImage",
       .binary = "eden.AppImage",
       .firmware = {{.id = "keys", .label = "Keys (prod.keys)", .dir = "~/.local/share/eden/keys",
                     .patterns = {"prod.keys"}},
                    {.id = "firmware", .label = "Firmware (a zip of .nca files)",
                     .dir = "~/.local/share/eden/nand/system/Contents/registered", .patterns = {"*.nca"}}}},
      {.id = "dolphin", .name = "Dolphin", .consoles = {"gamecube", "wii"},
       .extensions = {".iso", ".gcm", ".gcz", ".rvz", ".wbfs", ".wia", ".ciso", ".dol", ".elf", ".wad", ".tgc"},
       .args = {"-b", "-e", "{rom}"},
       .binary = "dolphin-emu"},
      {.id = "mesen", .name = "Mesen", .consoles = {"nes", "snes", "gb", "gbc", "pce", "sms", "gg"},
       .extensions = {".nes", ".fds", ".unf", ".sfc", ".smc", ".bs", ".gb", ".gbc", ".pce", ".sms", ".gg", ".zip"},
       .args = {"{rom}"},
       .release_url = GitHub("SourMesen/Mesen2"), .asset = "Mesen_*_Linux_x64.zip",
       .binary = "Mesen"},
      {.id = "snes9x", .name = "Snes9x", .consoles = {"snes"},
       .extensions = {".sfc", ".smc", ".fig", ".swc", ".bs", ".st", ".zip"},
       .args = {"{rom}"},
       .release_url = GitHub("snes9xgit/snes9x"), .asset = "Snes9x-*-x86_64.AppImage",
       .binary = "snes9x.AppImage"},
      {.id = "mgba", .name = "mGBA", .consoles = {"gb", "gbc", "gba"},
       .extensions = {".gba", ".gb", ".gbc", ".sgb", ".agb", ".zip", ".7z"},
       .args = {"{rom}"},
       .release_url = GitHub("mgba-emu/mgba"), .asset = "mGBA-*-appimage-x64.appimage",
       .binary = "mgba.AppImage"},
  };
  return emulators;
}

const Emulator* Find(std::string_view id) {
  const auto& all = All();
  const auto it = std::ranges::find(all, id, &Emulator::id);
  return it == all.end() ? nullptr : &*it;
}

const Emulator* ForGame(const model::Game& game) { return Find(game.source); }

namespace {

fs::path ToolDir(const config::Config& config, const Emulator& emulator) {
  return config.File().parent_path() / "tools" / emulator.id;
}

fs::path Expand(const std::string& dir) {
  return dir.starts_with("~/") ? paths::Home() / dir.substr(2) : fs::path(dir);
}

bool Accepts(const Emulator& emulator, const fs::path& file) {
  return std::ranges::contains(emulator.extensions, strings::ToLower(file.extension().string()));
}

bool IsFolderGame(const Emulator& emulator, const fs::path& dir) {
  std::error_code ec;
  return !emulator.folder_game.empty() && fs::is_regular_file(dir / emulator.folder_game, ec);
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

// The games in `root`: folder games, or files it takes that no .cue/.gdi/.m3u beside them names.
void FindGames(const Emulator& emulator, const fs::path& root, int depth, std::vector<Found>& found) {
  std::set<fs::path> named;
  std::vector<fs::path> files;
  for (const fs::path& entry : paths::ListDir(root)) {
    if (entry.filename().string().starts_with('.')) continue;
    std::error_code ec;
    if (fs::is_directory(entry, ec)) {
      if (IsFolderGame(emulator, entry)) found.push_back({entry, emulator.folder_game});
      else if (depth < 2) FindGames(emulator, entry, depth + 1, found);
      continue;
    }
    if (!Accepts(emulator, entry)) continue;
    files.push_back(entry);
    for (const fs::path& file : Named(entry)) named.insert(file.lexically_normal());
  }
  for (const fs::path& file : files) {
    if (!named.contains(file.lexically_normal())) found.push_back({root, file.filename().string()});
  }
}

std::string Slug(const Emulator& emulator, const fs::path& rom_dir, const Found& item) {
  fs::path rel = (item.dir / item.exe).lexically_relative(rom_dir);
  rel = emulator.folder_game.empty() ? rel.replace_extension() : (item.dir.lexically_relative(rom_dir));
  return strings::Slugify(rel.string());
}

fs::path MainFile(const model::Game& game) { return fs::path(game.install_path) / game.exe_path; }

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

}  // namespace

fs::path RomDir(const config::Config& config, const Emulator& emulator) {
  return config.GetPath("emulators.roms_root") / emulator.name;
}

fs::path Binary(const config::Config& config, const Emulator& emulator) {
  std::error_code ec;
  if (const fs::path chosen = config.GetPath(std::format("emulators.{}.bin", emulator.id)); !chosen.empty())
    return fs::is_regular_file(chosen, ec) ? chosen : fs::path();
  if (!emulator.release_url.empty()) {
    if (const fs::path managed = ToolDir(config, emulator) / emulator.binary; fs::is_regular_file(managed, ec))
      return managed;
  }
  if (auto found = runner::FindOnPath(emulator.binary)) return *found;
  return {};
}

std::string InstalledVersion(const config::Config& config, const Emulator& emulator) {
  std::ifstream in(ToolDir(config, emulator) / ".version");
  std::string tag;
  std::getline(in, tag);
  return tag;
}

Result<fs::path> Install(const config::Config& config, const Emulator& emulator) {
  if (emulator.release_url.empty()) {
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
  const Result<runner::ReleaseAsset> asset = runner::LatestRelease(emulator.release_url, emulator.asset);
  if (!asset) return std::unexpected(asset.error());
  auto installed = runner::InstallToolBinary(config, emulator.id, *asset, emulator.binary);
  if (!installed) return installed;
  std::ofstream(ToolDir(config, emulator) / ".version", std::ios::trunc) << asset->tag << "\n";
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
    if (emulator.id == "rpcs3") {
      const fs::path binary = Binary(config, emulator);
      if (binary.empty()) return Err("emulator_missing", "Install RPCS3 first, then add its firmware.");
      Command command;
      command.argv = {binary.string(), "--installfw", file.string()};
      const Result<runner::ExecResult> ran = runner::RunAndWait(command);
      if (!ran || ran->exit_code != 0) return Err("firmware_failed", "RPCS3 couldn't install that firmware file.");
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

std::vector<fs::path> GameFiles(const model::Game& game) {
  const Emulator* emulator = ForGame(game);
  if (emulator != nullptr && !emulator->folder_game.empty()) return {game.install_path};
  return WithNamed(MainFile(game));
}

Result<library::ImportSummary> AddFiles(config::Config& config, store::GameStore& games, api::EventBus& events,
                                        const Emulator& emulator, const std::vector<fs::path>& files, bool move) {
  const fs::path dir = RomDir(config, emulator);
  std::vector<std::string> refused;
  for (const fs::path& file : files) {
    std::error_code ec;
    const bool folder = fs::is_directory(file, ec);
    if (folder ? !IsFolderGame(emulator, file) : !Accepts(emulator, file)) {
      refused.push_back(file.filename().string());
      continue;
    }
    if (folder) {
      if (auto done = Transfer(file, dir / file.filename(), move); !done) return std::unexpected(done.error());
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

std::vector<const Emulator*> Alternatives(const config::Config& config, const model::Game& game) {
  const Emulator* from = ForGame(game);
  std::vector<const Emulator*> out;
  if (from == nullptr) return out;
  for (const Emulator& emulator : All()) {
    if (&emulator == from || Binary(config, emulator).empty()) continue;
    const bool shares = std::ranges::any_of(from->consoles, [&](const std::string& console) {
      return std::ranges::contains(emulator.consoles, console);
    });
    const bool takes = from->folder_game.empty() ? Accepts(emulator, game.exe_path)
                                                 : emulator.folder_game == from->folder_game;
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
  for (const fs::path& file : GameFiles(game)) {
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

  // Known games by their file, so a game moved here from another emulator keeps its id.
  std::map<fs::path, model::Game> by_file;
  for (const model::Game& game : games.All()) {
    if (game.source == emulator.id) by_file.emplace(MainFile(game).lexically_normal(), game);
  }

  library::ImportSummary summary;
  for (const Found& item : found) {
    const fs::path file = (item.dir / item.exe).lexically_normal();
    std::optional<model::Game> existing;
    if (const auto known = by_file.find(file); known != by_file.end()) existing = known->second;
    model::Game game = existing.value_or(model::Game{});
    if (!existing) {
      const std::string base = std::format("{}-{}", emulator.id, Slug(emulator, dir, item));
      game.id = base;
      for (int n = 2; games.Find(game.id); ++n) game.id = std::format("{}-{}", base, n);
      game.name = CleanName(emulator.folder_game.empty() ? item.exe : item.dir.filename().string());
    }
    game.source = emulator.id;
    game.install_path = item.dir.string();
    game.exe_path = item.exe;
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
    if (fs::exists(MainFile(game), ec)) continue;
    if (config.GetBool("library.remove_missing")) {
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
