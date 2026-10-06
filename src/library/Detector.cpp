#include "library/Detector.h"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>

#include "core/Strings.h"
#include "library/WinePrefix.h"

namespace mira::library {
namespace {

namespace fs = std::filesystem;

struct RawCandidate {
  fs::path rel_path;  // relative to the game folder
  model::Platform kind;
  int depth = 0;
  bool is_installer = false;
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
  return lower_ext.empty() || lower_ext == ".x86_64" || lower_ext == ".x86" || lower_ext == ".bin" ||
         lower_ext == ".run" || lower_ext == ".appimage";
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
  for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    std::error_code size_ec;
    if (entry.is_regular_file(size_ec) && fs::file_size(entry.path(), size_ec) >= min_bytes) return true;
  }
  return false;
}

// Name-pattern match plus a size signal: either the exe itself is large
// (a monolithic installer), or it shares a directory with a large payload
// file (a split installer). Name alone would flag legitimate small helpers
// too readily; size alone would miss both real packaging shapes above.
bool LooksLikeInstaller(const fs::path& path, const DetectorSettings& settings, bool dir_has_large_file) {
  if (!MatchesAny(settings.installer_name_patterns, path.filename().string())) return false;
  if (dir_has_large_file) return true;
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(path, ec);
  return !ec && size >= MinInstallerBytes(settings);
}

// Walks `folder` up to max_depth, skipping anything matching ignore_globs
// (relative to `folder`, so "*/prefix/*" excludes a prefix directory the
// caller nested inside the game folder, and a bare glob like ".*" excludes
// dotfiles at any depth).
std::vector<RawCandidate> WalkForExecutables(const fs::path& folder, const DetectorSettings& settings) {
  std::vector<RawCandidate> found;

  const std::uintmax_t min_installer_bytes = MinInstallerBytes(settings);
  // A game kept in its own prefix: the prefix's links (dosdevices/z: is /, users/<name>/Documents
  // is the real home) lead out of it and drive_c/windows is Wine's own, so neither is walked.
  const bool in_prefix = LooksLikeWinePrefix(folder);

  const std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
    if (depth > settings.max_depth) return;
    // Computed once per directory rather than per candidate: every
    // name-matching file in the same folder shares this signal. Skipped
    // entirely when no installer patterns are configured, since it costs a second
    // full directory pass plus a stat per entry, which for an asset-heavy
    // game folder roughly doubles scan I/O for nothing.
    const bool dir_has_large_file = !settings.installer_name_patterns.empty() &&
                                    DirectoryHasLargeFile(dir, min_installer_bytes);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
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
        found.push_back({rel, model::Platform::Windows, depth,
                         LooksLikeInstaller(entry.path(), settings, dir_has_large_file)});
      } else if (ext == ".msi") {
        found.push_back({rel, model::Platform::Windows, depth, /*is_installer=*/true});
      } else if (ext == ".sh" || HasExecuteBit(entry.path()) || (MightBeElf(ext) && LooksLikeElf(entry.path()))) {
        // A .sh is a candidate regardless of its executable bit (archives
        // routinely lose it); anything else needs the bit or ELF magic so a
        // stray data file doesn't get treated as a launcher.
        found.push_back({rel, model::Platform::Native, depth,
                         LooksLikeInstaller(entry.path(), settings, dir_has_large_file)});
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

  std::vector<double> scores(raw.size(), 0.0);
  const auto has_rule = [&](std::string_view name) {
    return std::ranges::find(settings_.rules, name) != settings_.rules.end();
  };

  if (has_rule("deny_patterns")) {
    for (size_t i = 0; i < raw.size(); ++i) {
      const std::string basename = raw[i].rel_path.filename().string();
      if (MatchesAny(settings_.deny_name_patterns, basename)) scores[i] -= 10.0;
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
  std::ranges::sort(order, [&](size_t a, size_t b) { return scores[a] > scores[b]; });

  Result result;
  result.candidates.reserve(raw.size());
  for (size_t rank = 0; rank < order.size(); ++rank) {
    const size_t i = order[rank];
    result.candidates.push_back({
        .rel_path = raw[i].rel_path.generic_string(),
        .kind = raw[i].kind,
        .score = scores[i],
        .chosen = rank == 0,
        .is_installer = raw[i].is_installer,
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
    const double name_component = 0.3 * strings::Similarity(
        fs::path(result.candidates[0].rel_path).stem().string(), cleaned_name);
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
