#include "library/Detector.h"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <functional>

#include "core/Strings.h"
#include "library/WinePrefix.h"

namespace mira::library {
namespace {

namespace fs = std::filesystem;

struct RawCandidate {
  fs::path rel_path;  // relative to the game folder
  model::Platform kind;
  int depth = 0;
  bool installer = false;
};

bool MatchesAny(const std::vector<std::string>& globs, std::string_view text) {
  return std::ranges::any_of(globs, [&](const std::string& glob) {
    return strings::GlobMatch(strings::ToLower(glob), strings::ToLower(text));
  });
}

// An ELF binary or a "#!" script, by its first bytes: the executable bit alone says nothing, since
// extraction tools drop it or set it on every file (a game's images included).
bool LooksLikeProgram(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  char magic[4] = {};
  file.read(magic, sizeof(magic));
  if (file.gcount() >= 2 && magic[0] == '#' && magic[1] == '!') return true;
  return file.gcount() == sizeof(magic) && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' &&
         magic[3] == 'F';
}

// Only extensionless files and the usual native-binary extensions are worth opening to check.
bool MightBeElf(const std::string& lower_ext) {
  return lower_ext.empty() || lower_ext == ".x86_64" || lower_ext == ".x86" ||
         lower_ext == ".bin" || lower_ext == ".run" || lower_ext == ".appimage";
}

bool HasExecuteBit(const fs::path& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR) != 0;
}

// Inno Setup's and NSIS's uninstallers are always named so.
bool IsUninstaller(const std::string& name) {
  const std::string lower = strings::ToLower(name);
  return lower.starts_with("unins") || lower.starts_with("uninst");
}

// A prerequisite or helper: denied by a pattern that isn't an installer's too ("setup*" is both).
bool IsHelper(const std::string& name, const DetectorSettings& settings) {
  return std::ranges::any_of(settings.deny_name_patterns, [&](const std::string& glob) {
    return !std::ranges::contains(settings.installer_name_patterns, glob) &&
           strings::GlobMatch(strings::ToLower(glob), strings::ToLower(name));
  });
}

// A hidden file (macOS's "._Game.exe" too) or a shared library, which may be marked executable.
bool NeverAProgram(const std::string& name) {
  return name.starts_with('.') || name.ends_with(".so") || name.find(".so.") != std::string::npos;
}

// A folder a game never starts from: hidden, "_"-prefixed (_CommonRedist, __MACOSX) or
// redistributables.
bool NeverHoldsTheGame(const std::string& dir_name) {
  if (dir_name.starts_with('.') || dir_name.starts_with('_')) return true;
  const std::string name = strings::ToLower(dir_name);
  const std::size_t at = name.find("redist");
  if (at == std::string::npos) return false;
  const std::string_view rest = std::string_view(name).substr(at + 6);
  return rest.empty() || rest == "s" || rest.starts_with("ributable") ||
         !std::isalpha(static_cast<unsigned char>(rest.front()));
}

// A name a game's documents go by (a readme, credits, a walkthrough, a link to its site).
bool IsDocumentName(std::string_view name) {
  const std::string lower = strings::ToLower(name);
  for (const char* document :
       {"readme", "read me", "manual", "licen", "credits", "changelog", "change log", "walkthrough",
        "eula", "faq", "guide", "docs", "notes", "welcome", "privacy", "website", "homepage"}) {
    if (lower.find(document) != std::string::npos) return true;
  }
  return false;
}

// Walks `folder` up to max_depth, skipping anything matching ignore_globs
// (relative to `folder`, so "*/prefix/*" excludes a prefix directory the
// caller nested inside the game folder, and a bare glob like ".*" excludes
// dotfiles at any depth).
std::vector<RawCandidate> WalkForExecutables(const fs::path& folder,
                                             const DetectorSettings& settings) {
  std::vector<RawCandidate> found;

  // A game kept in its own prefix: the prefix's links (dosdevices/z: is /, users/<name>/Documents
  // is the real home) lead out of it and drive_c/windows is Wine's own, so neither is walked.
  const bool in_prefix = LooksLikeWinePrefix(folder);

  const std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
    if (depth > settings.max_depth) return;
    const auto installer = [&](const fs::path& path) {
      return LooksLikeInstaller(path.filename().string(), settings);
    };
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
      // Lexical: fs::relative resolves symlinks, turning a linked subfolder into "../..".
      const fs::path rel = entry.path().lexically_relative(folder);
      if (MatchesAny(settings.ignore_globs, rel.generic_string())) continue;

      if (entry.is_directory(ec)) {
        if (NeverHoldsTheGame(entry.path().filename().string())) continue;
        // A wrapper folder can hold its actual prefix one level below itself
        // (umu's own layout: <root>/umu/umu-default/), and never descend into
        // one, or its own drive_c full of .exe files gets read as candidates.
        if (LooksLikeWinePrefix(entry.path())) continue;
        if (in_prefix && (entry.is_symlink(ec) || IsPrefixSystemDir(rel))) continue;
        walk(entry.path(), depth + 1);
        continue;
      }
      if (!entry.is_regular_file(ec) || NeverAProgram(entry.path().filename().string())) continue;

      const std::string ext = strings::ToLower(entry.path().extension().string());
      if (IsWebPage(entry.path())) {
        // A page at the top, or a folder's index.html one level down (www/, game/, not docs/).
        const bool placed =
            depth == 0 || (depth == 1 && entry.path().stem() == "index" &&
                           !IsDocumentName(entry.path().parent_path().filename().string()));
        if (placed && LooksLikeWebGame(entry.path()))
          found.push_back({rel, model::Platform::Web, depth, false});
      } else if (ext == ".exe" || ext == ".msi") {
        found.push_back({rel, model::Platform::Windows, depth, installer(entry.path())});
      } else if (ext == ".sh" || ((HasExecuteBit(entry.path()) || MightBeElf(ext)) &&
                                  LooksLikeProgram(entry.path()))) {
        // A .sh is a candidate whatever its bits (archives lose them); anything else has to start
        // like a program, so an image with the bit set isn't taken for one.
        found.push_back({rel, model::Platform::Native, depth, installer(entry.path())});
      }
    }
  };
  walk(folder, 0);
  return found;
}

double NameSimilarityBonus(const RawCandidate& candidate, const std::string& cleaned_folder_name,
                           double bonus) {
  const std::string stem = fs::path(candidate.rel_path).stem().string();
  return bonus * strings::Similarity(stem, cleaned_folder_name);
}

Detector::Result Rank(std::vector<RawCandidate> raw, const std::string& cleaned_name,
                      const DetectorSettings& settings) {
  // A page is the game only when nothing in the folder runs: a program beside one (NW.js games
  // keep theirs in www/, others ship a manual) is what starts the game.
  if (std::ranges::any_of(raw,
                          [](const RawCandidate& c) { return c.kind != model::Platform::Web; }))
    std::erase_if(raw, [](const RawCandidate& c) { return c.kind == model::Platform::Web; });
  std::vector<double> scores(raw.size(), 0.0);
  const auto has_rule = [&](std::string_view name) {
    return std::ranges::find(settings.rules, name) != settings.rules.end();
  };

  if (has_rule("deny_patterns")) {
    for (size_t i = 0; i < raw.size(); ++i) {
      // An installer ranks under any program, since it may be a leftover, and over the helpers.
      if (raw[i].installer) {
        scores[i] -= 5.0;
      } else if (MatchesAny(settings.deny_name_patterns, raw[i].rel_path.filename().string())) {
        scores[i] -= 10.0;
      }
    }
  }
  if (has_rule("name_similarity")) {
    for (size_t i = 0; i < raw.size(); ++i) {
      scores[i] += NameSimilarityBonus(raw[i], cleaned_name, settings.name_match_bonus);
    }
  }
  if (has_rule("depth")) {
    for (size_t i = 0; i < raw.size(); ++i) scores[i] -= settings.depth_penalty * raw[i].depth;
  }
  if (has_rule("shallowest") && !raw.empty()) {
    const int min_depth = std::ranges::min(raw, {}, &RawCandidate::depth).depth;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i].depth == min_depth) scores[i] += 1.0;
    }
  }

  std::vector<size_t> order(raw.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::ranges::sort(order, [&](size_t a, size_t b) { return scores[a] > scores[b]; });

  Detector::Result result;
  result.candidates.reserve(raw.size());
  for (size_t rank = 0; rank < order.size(); ++rank) {
    const size_t i = order[rank];
    result.candidates.push_back({
        .rel_path = raw[i].rel_path.generic_string(),
        .kind = raw[i].kind,
        .score = scores[i],
        .chosen = rank == 0,
        .is_installer = raw[i].installer,
    });
  }

  // Confidence: how much to trust the top pick. A clear score margin over
  // the runner-up, and a close name match to the folder, both raise it; a
  // single unopposed candidate gets a moderate baseline rather than full
  // confidence, since "the only thing found" is not the same guarantee as
  // "clearly the right one". These weights are deliberately simple: they
  // are meant to be retuned via detect.* config, not treated as final.
  if (result.candidates.empty()) {
    result.confidence = 0.0;
  } else {
    const double name_component =
        0.3 *
        strings::Similarity(fs::path(result.candidates[0].rel_path).stem().string(), cleaned_name);
    double margin_component;
    if (result.candidates.size() == 1) {
      margin_component = 0.4;
    } else {
      const double margin = result.candidates[0].score - result.candidates[1].score;
      margin_component = std::clamp(margin / 5.0, 0.0, 0.4);
    }
    result.confidence = std::clamp(0.3 + margin_component + name_component, 0.0, 1.0);
  }

  return result;
}

}  // namespace

// An uninstaller is named like an installer and a redistributable is one; neither is the game's.
bool LooksLikeInstaller(const std::string& name, const DetectorSettings& settings) {
  if (IsUninstaller(name) || IsHelper(name, settings)) return false;
  return strings::ToLower(fs::path(name).extension().string()) == ".msi" ||
         MatchesAny(settings.installer_name_patterns, name);
}

bool IsWebPage(const fs::path& file) {
  const std::string ext = strings::ToLower(file.extension().string());
  return ext == ".html" || ext == ".htm";
}

model::Platform PlatformOfFile(const fs::path& file) {
  const std::string ext = strings::ToLower(file.extension().string());
  if (ext == ".exe" || ext == ".msi" || ext == ".bat" || ext == ".cmd")
    return model::Platform::Windows;
  return IsWebPage(file) ? model::Platform::Web : model::Platform::Native;
}

bool LooksLikeWebGame(const fs::path& file) {
  if (IsDocumentName(file.stem().string())) return false;
  // A Twine game is one page tens of MB long; its head says what it is.
  constexpr std::size_t kHead = 256 * 1024;
  std::ifstream in(file, std::ios::binary);
  std::string head(kHead, '\0');
  in.read(head.data(), kHead);
  head.resize(static_cast<std::size_t>(in.gcount()));
  head = strings::ToLower(head);
  if (head.find("http-equiv=\"refresh\"") != std::string::npos ||
      head.find("http-equiv='refresh'") != std::string::npos ||
      head.find("http-equiv=refresh") != std::string::npos)
    return false;
  if (head.find("<script") == std::string::npos && head.find("<canvas") == std::string::npos)
    return false;
  // A short page whose script only goes somewhere else.
  std::error_code ec;
  const bool short_page = fs::file_size(file, ec) < 8 * 1024;
  const bool sends_away = head.find("location.href") != std::string::npos ||
                          head.find("location.replace") != std::string::npos ||
                          head.find("window.location") != std::string::npos ||
                          head.find("window.open") != std::string::npos;
  return !(short_page && sends_away);
}

Detector::Result Detector::Detect(const fs::path& folder, const std::string& name) const {
  return Rank(WalkForExecutables(folder, settings_),
              strings::CleanGameName(name.empty() ? folder.filename().string() : name), settings_);
}

Detector::Result Detector::DetectListed(const std::vector<fs::path>& files,
                                        const std::string& name) const {
  std::vector<RawCandidate> raw;
  for (const fs::path& file : files) {
    if (NeverAProgram(file.filename().string())) continue;
    const int depth = static_cast<int>(std::distance(file.begin(), file.end())) - 1;
    if (depth < 0 || depth > settings_.max_depth || file.is_absolute()) continue;
    bool skipped = MatchesAny(settings_.ignore_globs, file.generic_string());
    fs::path folder;
    for (const fs::path& part : file.parent_path()) {
      folder /= part;
      skipped = skipped || part == ".." || NeverHoldsTheGame(part.string()) ||
                MatchesAny(settings_.ignore_globs, folder.generic_string());
    }
    if (skipped) continue;
    const std::string ext = strings::ToLower(file.extension().string());
    const bool windows = ext == ".exe" || ext == ".msi";
    if (!windows && ext != ".sh" && ext != ".appimage" && ext != ".x86_64" && ext != ".x86")
      continue;
    raw.push_back({file, windows ? model::Platform::Windows : model::Platform::Native, depth,
                   LooksLikeInstaller(file.filename().string(), settings_)});
  }
  return Rank(std::move(raw), strings::CleanGameName(name), settings_);
}
DetectorSettings SettingsFromConfig(const config::Config& config) {
  DetectorSettings settings;
  settings.max_depth = static_cast<int>(config.GetInt("scan.max_depth"));
  settings.rules = config.GetStringArray("detect.rules");
  settings.name_match_bonus = config.GetDouble("detect.name_match_bonus");
  settings.depth_penalty = config.GetDouble("detect.depth_penalty");
  settings.deny_name_patterns = config.GetStringArray("detect.deny_name_patterns");
  settings.installer_name_patterns = config.GetStringArray("detect.installer_name_patterns");
  settings.ignore_globs = config.GetStringArray("scan.ignore_globs");
  return settings;
}

}  // namespace mira::library
