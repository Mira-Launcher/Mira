#include "library/ProgramScore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <optional>
#include <vector>

#include "core/Strings.h"

namespace mira::library {
namespace {

namespace fs = std::filesystem;

using Words = std::vector<std::string>;

// A Roman numeral's value as digits, for a sequel's number. "i" alone is a word, and so is "mix".
std::string AsNumber(const std::string& word) {
  constexpr std::array<std::string_view, 14> kRoman = {
      "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x", "xi", "xii", "xiii", "xiv", "xv"};
  const auto found = std::ranges::find(kRoman, word);
  return found == kRoman.end() ? word : std::to_string(found - kRoman.begin() + 2);
}

// Lowercase words: split at anything that isn't a letter or digit, between a letter and a digit,
// and where a capital starts a word ("BloonsTD6" is bloons, td, 6).
Words SplitWords(std::string_view text) {
  Words words;
  std::string word;
  const auto end = [&] {
    if (!word.empty()) words.push_back(AsNumber(word));
    word.clear();
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '\'') continue;  // Assassin's
    if (!std::isalnum(c) && c < 0x80) {
      end();
      continue;
    }
    if (!word.empty()) {
      const auto before = static_cast<unsigned char>(text[i - 1]);
      const auto after = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
      const bool number_edge = (std::isdigit(c) != 0) != (std::isdigit(before) != 0);
      const bool capital = std::isupper(c) && (std::islower(before) || std::islower(after));
      if (number_edge || capital) end();
    }
    word += static_cast<char>(std::tolower(c));
  }
  end();
  return words;
}

std::string Join(const Words& words) {
  std::string out;
  for (const std::string& word : words) out += word;
  return out;
}

// Words a build adds to a program's name, or a folder to a game's.
bool IsBuildWord(const std::string& word) {
  constexpr std::array<std::string_view, 33> kWords = {
      "win",  "windows", "win32",   "win64",  "wingdk", "x64",      "x86",   "x",   "64",
      "32",   "86",      "bit",     "amd",    "linux",  "lin",      "unix",  "pc",  "exe",
      "bin",  "app",     "release", "retail", "final",  "shipping", "steam", "gog", "egs",
      "epic", "dx",      "dx11",    "dx12",   "vulkan", "opengl"};
  return std::ranges::find(kWords, word) != kWords.end();
}

Words WithoutBuildWords(Words words) {
  Words kept;
  for (std::string& word : words) {
    if (!IsBuildWord(word)) kept.push_back(std::move(word));
  }
  return kept;
}

// Each word's first letter, numbers whole: "ac2".
std::string Initials(const Words& words) {
  std::string out;
  for (const std::string& word : words)
    out += std::isdigit(static_cast<unsigned char>(word[0])) ? word : word.substr(0, 1);
  return out;
}

// Whether `part`'s letters are all in `whole`, in order, as in a name with its vowels left out.
bool Shortens(std::string_view part, std::string_view whole) {
  if (part.empty() || whole.empty() || part[0] != whole[0]) return false;
  std::size_t at = 0;
  for (const char c : whole) {
    if (at < part.size() && part[at] == c) ++at;
  }
  return at == part.size();
}

std::string Lower(const fs::path& path) {
  return strings::ToLower(path.generic_string());
}

bool HasEnding(std::string_view name, std::initializer_list<std::string_view> endings) {
  return std::ranges::any_of(endings, [&](std::string_view end) { return name.ends_with(end); });
}

}  // namespace

double ProgramNameMatch(std::string_view stem, std::string_view game) {
  const Words program = WithoutBuildWords(SplitWords(stem));
  Words title = WithoutBuildWords(SplitWords(game));
  if (title.empty()) title = SplitWords(game);  // "Windows 93"
  if (program.empty() || title.empty()) return 0.0;
  const std::string a = Join(program);
  const std::string b = Join(title);
  if (a == b) return 1.0;

  double best = 0.0;
  if (std::min(a.size(), b.size()) >= 3 && (a.contains(b) || b.contains(a))) {
    const double shorter = static_cast<double>(std::min(a.size(), b.size()));
    best = 0.5 + 0.5 * shorter / static_cast<double>(std::max(a.size(), b.size()));
  }
  // "ac2", "ffxv_s": two letters could be anything's.
  const std::string initials = Initials(title);
  if (initials.size() >= 2 && (a == initials || (a.size() > 2 && a.starts_with(initials))))
    best = std::max(best, initials.size() > 2 ? 0.8 : 0.6);
  std::size_t shared = 0;
  Words left = title;
  for (const std::string& word : program) {
    if (const auto found = std::ranges::find(left, word); found != left.end()) {
      shared += word.size();
      left.erase(found);
    }
  }
  best =
      std::max(best, 2.0 * static_cast<double>(shared) / static_cast<double>(a.size() + b.size()));
  if (a.size() >= 3 && Shortens(a, b)) best = std::max(best, 0.45);
  std::size_t common = 0;
  while (common < a.size() && common < b.size() && a[common] == b[common]) ++common;
  if (common >= 3)
    best = std::max(best, 0.8 * static_cast<double>(common) /
                              static_cast<double>(std::max(a.size(), b.size())));
  return best;
}

bool HasEngineFiles(const fs::path& rel_path, const FolderContents& contents) {
  const std::string dir = Lower(rel_path.parent_path());
  const std::string at = dir.empty() ? "" : dir + "/";
  const std::string name = strings::ToLower(rel_path.filename().string());
  const std::string stem = strings::ToLower(rel_path.stem().string());
  const auto has = [&](const std::string& path) { return contents.contains(at + path); };
  // Anything in the folder `path` is in, whether or not the folder itself was listed.
  const auto holds = [&](const std::string& path) {
    const auto found = contents.lower_bound(at + path + "/");
    return has(path) || (found != contents.end() && found->starts_with(at + path + "/"));
  };
  if (holds(stem + "_data")) return true;                // Unity
  if (has(stem + ".pck")) return true;                   // Godot
  if (has(stem + ".py") && holds("renpy")) return true;  // Ren'Py
  if (stem.ends_with("-shipping")) {
    // Unreal's game itself. The program at the top that starts it is better.
    const fs::path top = rel_path.parent_path().parent_path().parent_path().parent_path();
    const std::string project =
        Lower(rel_path.parent_path().parent_path().parent_path().filename());
    return !contents.contains(Lower(top / (project + ".exe")));
  }
  const std::string base = stem.substr(0, stem.find('.'));
  if (has(base + ".exe") && (has("fna.dll") || has("monogame.framework.dll"))) return true;
  if (holds("engine") && holds(stem + "/binaries")) return true;  // Unreal's starter
  const bool crash_handler = stem.starts_with("unitycrashhandler") || stem == "crashpad_handler";
  if (crash_handler || stem == "chrome_crashpad_handler" || stem == "notification_helper")
    return false;
  if (HasEnding(name, {".exe"})) {
    if (has("data.win") || has("game.unx")) return true;  // GameMaker
    if (has("rpg_rt.ldb") || has("game.rgss3a") || has("game.rgss2a") || has("game.rgssad"))
      return true;                                                         // RPG Maker
    if (has("nw.pak") || has("nw_100_percent.pak")) return true;           // NW.js
    if (has("resources/app.asar") || holds("resources/app")) return true;  // Electron
    if (has("data.xp3")) return true;                                      // KiriKiri
  }
  return false;
}

double HelperPenalty(const fs::path& rel_path, std::string_view game) {
  struct Word {
    std::string_view text;
    double penalty;
  };
  // A launcher or a config tool may be how a game is started, so they lose to a likely game only.
  constexpr std::array<Word, 43> kWords = {{
      {"launcher", 1.5},    {"launch", 0.5},    {"config", 3.0},   {"configuration", 3.0},
      {"configure", 3.0},   {"settings", 3.0},  {"options", 3.0},  {"setup", 2.0},
      {"server", 3.0},      {"dedicated", 3.0}, {"editor", 3.0},   {"sdk", 3.0},
      {"mod", 2.0},         {"modding", 3.0},   {"tool", 3.0},     {"tools", 3.0},
      {"toolkit", 3.0},     {"benchmark", 3.0}, {"crash", 4.0},    {"reporter", 4.0},
      {"report", 3.0},      {"dump", 4.0},      {"update", 3.0},   {"updater", 4.0},
      {"patch", 3.0},       {"patcher", 4.0},   {"cleaner", 4.0},  {"handler", 4.0},
      {"helper", 4.0},      {"service", 3.0},   {"overlay", 3.0},  {"uninstall", 4.0},
      {"install", 3.0},     {"installer", 3.0}, {"register", 3.0}, {"registration", 3.0},
      {"activation", 3.0},  {"browser", 3.0},   {"trainer", 3.0},  {"converter", 3.0},
      {"diagnostics", 3.0}, {"test", 1.5},      {"debug", 2.0},
  }};
  // Programs a game carries that are something else's.
  constexpr std::array<std::string_view, 24> kOthers = {
      "ffmpeg",     "ffprobe",    "python",     "pythonw", "node",     "java",
      "javaw",      "7z",         "7za",        "unrar",   "curl",     "wget",
      "zsync",      "zsyncmake",  "createdump", "elevate", "quicksfv", "upc",
      "cefprocess", "subprocess", "dosbox",     "scummvm", "steamcmd", "oggenc"};
  constexpr std::array<std::string_view, 33> kFolders = {
      "tools",      "tool",        "support",         "utils",   "util",    "utilities",
      "docs",       "doc",         "manual",          "extras",  "bonus",   "mods",
      "engine",     "lib",         "lib64",           "libs",    "jre",     "runtime",
      "python",     "plugins",     "editor",          "server",  "sdk",     "patch",
      "installer",  "support",     "crack",           "cleaner", "directx", "dotnet",
      "thirdparty", "third_party", "monobleedingedge"};

  const Words title = SplitWords(game);
  const std::string stem = strings::ToLower(rel_path.stem().string());
  const std::string joined = Join(title);
  double penalty = 0.0;
  if (std::ranges::find(kOthers, stem) != kOthers.end() && stem != joined) penalty = 4.0;
  for (const std::string& word : SplitWords(rel_path.stem().string())) {
    if (std::ranges::find(title, word) != title.end()) continue;
    for (const Word& helper : kWords) {
      // "CrashSender", and "gamelauncher" run together.
      if (word == helper.text || (helper.text.size() >= 6 && word.ends_with(helper.text)))
        penalty = std::max(penalty, helper.penalty);
    }
  }
  for (const fs::path& part : rel_path.parent_path()) {
    const std::string folder = strings::ToLower(part.string());
    const Words words = SplitWords(part.string());
    if (Join(words) == joined) continue;
    if (folder.ends_with("_data") || std::ranges::find(kFolders, folder) != kFolders.end())
      penalty = std::max(penalty, 3.0);
  }
  return penalty;
}

int CountedDepth(const fs::path& rel_path, std::string_view game) {
  constexpr std::array<std::string_view, 17> kNeutral = {
      "bin",     "bin64", "bin32",   "binaries", "win64", "win32", "x64",   "x86",   "win",
      "windows", "linux", "linux64", "wingdk",   "game",  "app",   "build", "system"};
  int depth = 0;
  for (const fs::path& part : rel_path.parent_path()) {
    const std::string folder = strings::ToLower(part.string());
    if (std::ranges::find(kNeutral, folder) != kNeutral.end()) continue;
    if (ProgramNameMatch(folder, game) >= 0.8) continue;
    ++depth;
  }
  return depth;
}

bool Is32Bit(const fs::path& rel_path) {
  const std::string stem = strings::ToLower(rel_path.filename().string());
  if (stem.find("x86_64") != std::string::npos || stem.find("64") != std::string::npos)
    return false;
  if (HasEnding(strings::ToLower(rel_path.stem().string()), {"32", "x86", "win32"}) ||
      stem.ends_with(".x86"))
    return true;
  return std::ranges::any_of(rel_path.parent_path(), [](const fs::path& part) {
    const std::string folder = strings::ToLower(part.string());
    return folder == "win32" || folder == "x86" || folder == "bin32" || folder == "32bit";
  });
}

bool HasStarter(const fs::path& rel_path, const FolderContents& contents) {
  const std::string name = strings::ToLower(rel_path.filename().string());
  if (!HasEnding(name, {".x86", ".x86_64", ".bin"})) return false;
  const std::string base = Lower(rel_path.parent_path() / name.substr(0, name.find('.')));
  return contents.contains(base) || contents.contains(base + ".sh");
}

double SizeScore(std::int64_t bytes) {
  if (bytes <= 0) return 0.0;
  return std::clamp(std::log2(static_cast<double>(bytes) / (64.0 * 1024)) / 10.0, 0.0, 1.0);
}

bool IsConsoleProgram(const fs::path& exe) {
  std::ifstream in(exe, std::ios::binary);
  const auto number = [&](std::streamoff at, int bytes) -> std::optional<std::uint32_t> {
    std::array<unsigned char, 4> raw{};
    in.seekg(at);
    if (!in.read(reinterpret_cast<char*>(raw.data()), bytes)) return std::nullopt;
    return raw[0] | raw[1] << 8 | raw[2] << 16 | static_cast<std::uint32_t>(raw[3]) << 24;
  };
  if (number(0, 2) != 0x5a4d) return false;  // MZ
  const auto header = number(0x3c, 4);
  if (!header || number(*header, 4) != 0x4550) return false;  // PE
  return number(static_cast<std::streamoff>(*header) + 24 + 68, 2) == 3;
}

}  // namespace mira::library
