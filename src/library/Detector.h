#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config/Config.h"
#include "model/Types.h"

namespace mira::library {

// Every knob here mirrors a `detect.*` schema key (src/config/Schema.cpp),
// Detector takes plain values rather than a config::Config so it stays
// testable with no settings file involved, and callers (Scanner) are the
// only place that has to know these come from config at all.
struct DetectorSettings {
  int max_depth = 4;
  std::vector<std::string> rules = {"deny_patterns", "name_similarity", "depth", "shallowest"};
  double name_match_bonus = 3.0;
  double depth_penalty = 0.5;
  double low_confidence_threshold = 0.5;
  std::vector<std::string> deny_name_patterns;
  std::vector<std::string> installer_name_patterns;
  std::int64_t installer_min_size_mb = 50;
  // Applied during the walk itself (skips whole subtrees), distinct from
  // deny_name_patterns which only penalises a candidate's score. Should
  // include scan.ignore_globs plus the caller's prefix_root exclusion.
  std::vector<std::string> ignore_globs;
};

// Reads every detect.*/scan.max_depth/scan.ignore_globs key into one
// DetectorSettings -- shared by Scanner (a fresh scan) and AutoInstall (a
// post-install re-detect), so both score/flag candidates identically.
DetectorSettings SettingsFromConfig(const config::Config& config);

// Scores the executables inside one already-identified game folder. It does
// not discover game folders itself (that is Scanner's job) and does not know
// about runners or prefixes. `detect.rules` selects which of the passes
// below run, and in what order: an unrecognised or omitted name simply does
// nothing, which is what lets a user disable one by editing the list rather
// than needing a code change.
class Detector {
 public:
  struct Result {
    std::vector<model::Candidate> candidates;  // sorted best first
    double confidence = 0.0;                   // 0..1; see Detector.cpp for how this is derived
  };

  explicit Detector(DetectorSettings settings) : settings_(std::move(settings)) {}

  Result Detect(const std::filesystem::path& folder) const;

 private:
  DetectorSettings settings_;
};

}  // namespace mira::library
