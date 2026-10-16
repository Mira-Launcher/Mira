#include "library/Detector.h"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <optional>

#include "core/Strings.h"
#include "library/AutoInstall.h"
#include "library/WinePrefix.h"

namespace mira::library {
namespace {

namespace fs = std::filesystem;

enum class InstallerKind {
  kNone,
  kInstaller,  // an installer, which may be a leftover beside the installed game
  kPackage,    // an installer with its payload beside it: the folder isn't installed yet
};

struct RawCandidate {
  fs::path rel_path;  // relative to the game folder
  model::Platform kind;
  int depth = 0;
  InstallerKind installer = InstallerKind::kNone;
};

bool MatchesAny(const std::vector<std::string>& globs, std::string_view text) {
  return std::ranges::any_of(globs, [&](const std::string& glob) {
    return strings::GlobMatch(strings::ToLower(glob), strings::ToLower(text));
  });
}

// True for a regular file whose first four bytes are the ELF magic number:
// used instead of relying solely on the executable bit, which extraction
// tools frequently drop or set inconsistently.
bool LooksLikeElf(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  char magic[4] = {};
  file.read(magic, sizeof(magic));
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

std::uintmax_t MinInstallerBytes(const DetectorSettings& settings) {
  return static_cast<std::uintmax_t>(settings.installer_min_size_mb) * 1024 * 1024;
}

// True if any file directly in `dir` meets the size floor. Installers are
// frequently a small stub .exe plus a much larger separate payload (a
// classic InstallShield/Inno Setup split: a few-MB launcher next to a
// multi-hundred-MB .bin/.cab), so checking only the exe's own size misses
// this real, common packaging shape entirely.
bool DirectoryHasLargeFile(const fs::path& dir, std::uintmax_t min_bytes) {
  std::error_code ec;
  for (const auto& entry :
       fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    std::error_code size_ec;
    if (entry.is_regular_file(size_ec) && fs::file_size(entry.path(), size_ec) >= min_bytes)
      return true;
  }
  return false;
}

// Inno Setup's and NSIS's uninstallers carry their builder's mark too, and are always named so.
bool IsUninstaller(const std::string& name) {
  const std::string lower = strings::ToLower(name);
  return lower.starts_with("unins") || lower.starts_with("uninst");
}

// An installer builder's mark with its payload beside it (or making up the file itself) is a
// package, whatever its name. A marked file without one is an installer unless the name says it's
// a redistributable or helper (they carry the same marks). Failing a mark, an installer's name plus
// that size signal. Only shallow files are opened for a mark, since that reads up to 4 MB of each.
InstallerKind ClassifyInstaller(const fs::path& path, const DetectorSettings& settings,
                                const std::function<bool()>& dir_has_large_file, int depth) {
  const std::string name = path.filename().string();
  if (IsUninstaller(name)) return InstallerKind::kNone;
  const bool named = MatchesAny(settings.installer_name_patterns, name);
  if ((named || depth <= 1) && IsBuiltInstaller(path)) {
    if (dir_has_large_file()) return InstallerKind::kPackage;
    if (!MatchesAny(settings.deny_name_patterns, name)) return InstallerKind::kInstaller;
  }
  if (!named) return InstallerKind::kNone;
  if (dir_has_large_file()) return InstallerKind::kInstaller;
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  return !ec && size >= MinInstallerBytes(settings) ? InstallerKind::kInstaller
                                                    : InstallerKind::kNone;
}

// Walks `folder` up to max_depth, skipping anything matching ignore_globs
// (relative to `folder`, so "*/prefix/*" excludes a prefix directory the
// caller nested inside the game folder, and a bare glob like ".*" excludes
// dotfiles at any depth).
std::vector<RawCandidate> WalkForExecutables(const fs::path& folder,
                                             const DetectorSettings& settings) {
  std::vector<RawCandidate> found;

  const std::uintmax_t min_installer_bytes = MinInstallerBytes(settings);
  // A game kept in its own prefix: the prefix's links (dosdevices/z: is /, users/<name>/Documents
  // is the real home) lead out of it and drive_c/windows is Wine's own, so neither is walked.
  const bool in_prefix = LooksLikeWinePrefix(folder);

  const std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
    if (depth > settings.max_depth) return;
    // Computed once per directory, and only when a file in it asks: it costs a second full
    // directory pass plus a stat per entry, which for an asset-heavy game folder roughly doubles
    // scan I/O.
    std::optional<bool> has_large_file;
    const std::function<bool()> dir_has_large_file = [&] {
      if (!has_large_file) has_large_file = DirectoryHasLargeFile(dir, min_installer_bytes);
      return *has_large_file;
    };
    const auto installer = [&](const fs::path& path) {
      return ClassifyInstaller(path, settings, dir_has_large_file, depth);
    };
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
      // Lexical: fs::relative resolves symlinks, turning a linked subfolder into "../..".
      const fs::path rel = entry.path().lexically_relative(folder);
      if (MatchesAny(settings.ignore_globs, rel.generic_string())) continue;

      if (entry.is_directory(ec)) {
        // A wrapper folder can hold its actual prefix one level below itself
        // (umu's own layout: <root>/umu/umu-default/), and never descend into
        // one, or its own drive_c full of .exe files gets read as candidates.
        if (LooksLikeWinePrefix(entry.path())) continue;
        if (in_prefix && (entry.is_symlink(ec) || IsPrefixSystemDir(rel))) continue;
        walk(entry.path(), depth + 1);
        continue;
      }
      if (!entry.is_regular_file(ec)) continue;

      const std::string ext = strings::ToLower(entry.path().extension().string());
      if (ext == ".exe") {
        found.push_back({rel, model::Platform::Windows, depth, installer(entry.path())});
      } else if (ext == ".msi") {
        found.push_back({rel, model::Platform::Windows, depth, InstallerKind::kInstaller});
      } else if (ext == ".sh" || HasExecuteBit(entry.path()) ||
                 (MightBeElf(ext) && LooksLikeElf(entry.path()))) {
        // A .sh is a candidate regardless of its executable bit (archives
        // routinely lose it); anything else needs the bit or ELF magic so a
        // stray data file doesn't get treated as a launcher.
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

}  // namespace

Detector::Result Detector::Detect(const fs::path& folder) const {
  const std::string cleaned_name = strings::CleanGameName(folder.filename().string());
  std::vector<RawCandidate> raw = WalkForExecutables(folder, settings_);
  // A plain executable at a package's level or above it means the game is installed, and the
  // package is one of its leftovers or redistributables (VC_redist carries a WiX mark too).
  for (RawCandidate& candidate : raw) {
    if (candidate.installer != InstallerKind::kPackage) continue;
    const bool installed = std::ranges::any_of(raw, [&](const RawCandidate& other) {
      return other.installer == InstallerKind::kNone && other.depth <= candidate.depth &&
             !MatchesAny(settings_.deny_name_patterns, other.rel_path.filename().string());
    });
    if (installed) candidate.installer = InstallerKind::kInstaller;
  }

  std::vector<double> scores(raw.size(), 0.0);
  const auto has_rule = [&](std::string_view name) {
    return std::ranges::find(settings_.rules, name) != settings_.rules.end();
  };

  const auto package = [&](size_t i) { return raw[i].installer == InstallerKind::kPackage; };
  if (has_rule("deny_patterns")) {
    for (size_t i = 0; i < raw.size(); ++i) {
      const std::string basename = raw[i].rel_path.filename().string();
      // The list names helpers by guess; a package's mark and payload say what it is.
      if (!package(i) && MatchesAny(settings_.deny_name_patterns, basename)) scores[i] -= 10.0;
    }
  }
  if (has_rule("name_similarity")) {
    for (size_t i = 0; i < raw.size(); ++i) {
      scores[i] += NameSimilarityBonus(raw[i], cleaned_name, settings_.name_match_bonus);
    }
  }
  if (has_rule("depth")) {
    for (size_t i = 0; i < raw.size(); ++i) scores[i] -= settings_.depth_penalty * raw[i].depth;
  }
  if (has_rule("shallowest") && !raw.empty()) {
    const int min_depth = std::ranges::min(raw, {}, &RawCandidate::depth).depth;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i].depth == min_depth) scores[i] += 1.0;
    }
  }

  std::vector<size_t> order(raw.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  // A package means nothing is installed yet, so nothing beside it (a checksum tool, a
  // redistributable) is the game: it comes first.
  std::ranges::sort(order, [&](size_t a, size_t b) {
    if (package(a) != package(b)) return package(a);
    return scores[a] > scores[b];
  });

  Result result;
  result.candidates.reserve(raw.size());
  for (size_t rank = 0; rank < order.size(); ++rank) {
    const size_t i = order[rank];
    result.candidates.push_back({
        .rel_path = raw[i].rel_path.generic_string(),
        .kind = raw[i].kind,
        .score = scores[i],
        .chosen = rank == 0,
        .is_installer = raw[i].installer != InstallerKind::kNone,
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
  } else if (package(order[0])) {
    result.confidence = 1.0;
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

DetectorSettings SettingsFromConfig(const config::Config& config) {
  DetectorSettings settings;
  settings.max_depth = static_cast<int>(config.GetInt("scan.max_depth"));
  settings.rules = config.GetStringArray("detect.rules");
  settings.name_match_bonus = config.GetDouble("detect.name_match_bonus");
  settings.depth_penalty = config.GetDouble("detect.depth_penalty");
  settings.low_confidence_threshold = config.GetDouble("detect.low_confidence_threshold");
  settings.deny_name_patterns = config.GetStringArray("detect.deny_name_patterns");
  settings.installer_name_patterns = config.GetStringArray("detect.installer_name_patterns");
  settings.installer_min_size_mb = config.GetInt("detect.installer_min_size_mb");
  settings.ignore_globs = config.GetStringArray("scan.ignore_globs");
  return settings;
}

}  // namespace mira::library
