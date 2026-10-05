#include "core/Strings.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace mira::strings {
namespace {

// Tokens that appear in repack and store directory names but never in the
// game's actual title. Matched case-insensitively as whole tokens.
constexpr std::string_view kNoiseTokens[] = {
    "repack",   "gog",      "goty",   "codex",    "plaza",   "skidrow",  "fitgirl",
    "dodi",     "elamigos", "rune",   "empress",  "tenoke",  "razor1911", "flt",
    "ankergames", "steamrip", "multi", "proper",  "incl",    "dlc",      "update",
    "crack",    "cracked",  "portable", "win64",  "win32",   "x64",      "x86",
    "edition",  "deluxe",   "complete", "remastered", "definitive",
};

bool IsNoiseToken(std::string_view token) {
  const std::string lower = ToLower(token);
  return std::ranges::find(kNoiseTokens, lower) != std::end(kNoiseTokens);
}

// A multi-part version number ("v1.4.0.0", "1.20.3") is noise wherever it
// appears in the raw name; a bare number ("2020", "2") is not, since plenty
// of real titles end in one (Trackmania 2020, Half-Life 2, Portal 2). The
// distinguishing signal is the embedded dot, so this has to run on the raw
// string before generic dot-to-space normalisation would erase it.
std::string StripVersionNumbers(std::string_view input) {
  static const std::regex kVersion(R"(v?\d+(\.\d+)+)", std::regex::icase);
  return std::regex_replace(std::string(input), kVersion, " ");
}

}  // namespace

std::string ToLower(std::string_view input) {
  std::string out(input);
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string Trim(std::string_view input) {
  const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!input.empty() && is_space(input.front())) input.remove_prefix(1);
  while (!input.empty() && is_space(input.back())) input.remove_suffix(1);
  return std::string(input);
}

std::vector<std::string> Split(std::string_view input, char delimiter) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= input.size()) {
    const size_t end = input.find(delimiter, start);
    if (end == std::string_view::npos) {
      parts.emplace_back(input.substr(start));
      break;
    }
    parts.emplace_back(input.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

bool GlobMatch(std::string_view pattern, std::string_view text) {
  size_t p = 0, t = 0, star = std::string_view::npos, retry = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      retry = t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++retry;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

std::string Slugify(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  bool last_was_dash = false;
  for (const unsigned char c : input) {
    if (std::isalnum(c)) {
      out += static_cast<char>(std::tolower(c));
      last_was_dash = false;
    } else if (!last_was_dash && !out.empty()) {
      out += '-';
      last_was_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "game" : out;
}

std::string CleanGameName(std::string_view directory_name) {
  std::string working = StripVersionNumbers(directory_name);
  std::ranges::replace(working, '_', ' ');
  std::ranges::replace(working, '.', ' ');

  // Hyphens separate tokens in repack names but also appear inside real
  // titles, so only split on them when a noise token follows.
  std::string spaced;
  for (size_t i = 0; i < working.size(); ++i) {
    spaced += (working[i] == '-') ? ' ' : working[i];
  }

  std::vector<std::string> kept;
  for (const std::string& token : Split(spaced, ' ')) {
    const std::string trimmed = Trim(token);
    if (trimmed.empty()) continue;
    if (IsNoiseToken(trimmed)) continue;
    // "(64bit)", "(12345)": GOG installers' build and architecture tags.
    if (trimmed.starts_with('(') && trimmed.ends_with(')')) continue;
    kept.push_back(trimmed);
  }
  // An installer's folder ("setup_crate_escape") names the game after its first word.
  if (kept.size() > 1) {
    const std::string first = ToLower(kept.front());
    if (first == "setup" || first == "install" || first == "installer") kept.erase(kept.begin());
  }
  if (kept.empty()) return Trim(directory_name);

  std::string out;
  for (const std::string& token : kept) {
    if (!out.empty()) out += ' ';
    out += token;
  }
  // An all-lowercase folder name reads as a title once each word is capitalised; any capital means
  // the casing was chosen ("inFAMOUS") and stays.
  if (std::ranges::none_of(out, [](unsigned char c) { return std::isupper(c); })) {
    bool word_start = true;
    for (char& c : out) {
      if (word_start && std::isalpha(static_cast<unsigned char>(c))) c = static_cast<char>(std::toupper(c));
      word_start = c == ' ';
    }
  }
  return out;
}

double Similarity(std::string_view a, std::string_view b) {
  const std::string lhs = ToLower(a), rhs = ToLower(b);
  if (lhs.empty() || rhs.empty()) return 0.0;
  if (lhs == rhs) return 1.0;

  // Containment scores highly: "CelesteLauncher" in a "Celeste" folder is a
  // strong signal, and a full edit distance would under-rate it.
  if (lhs.contains(rhs) || rhs.contains(lhs)) {
    const double shorter = static_cast<double>(std::min(lhs.size(), rhs.size()));
    const double longer = static_cast<double>(std::max(lhs.size(), rhs.size()));
    return 0.5 + (0.5 * (shorter / longer));
  }

  // Otherwise fall back to a cheap common-prefix ratio.
  size_t common = 0;
  while (common < lhs.size() && common < rhs.size() && lhs[common] == rhs[common]) ++common;
  return static_cast<double>(common) / static_cast<double>(std::max(lhs.size(), rhs.size()));
}

}  // namespace mira::strings
