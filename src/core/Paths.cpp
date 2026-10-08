#include "core/Paths.h"

#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mira::paths {
namespace {

std::filesystem::path EnvOr(const char* name, const std::filesystem::path& fallback) {
  const char* value = std::getenv(name);
  return (value && *value) ? std::filesystem::path(value) : fallback;
}

}  // namespace

std::filesystem::path Home() {
  if (const char* home = std::getenv("HOME"); home && *home) return home;
  if (const passwd* pw = ::getpwuid(::getuid())) return pw->pw_dir;
  return "/tmp";
}

std::filesystem::path UserDir() { return EnvOr("XDG_CONFIG_HOME", Home() / ".config") / "mira"; }

std::filesystem::path RuntimeDir() {
  // XDG_RUNTIME_DIR is absent under some session managers; /tmp keeps the
  // daemon startable rather than failing over a socket location.
  return EnvOr("XDG_RUNTIME_DIR", std::filesystem::path("/tmp")) / "mira";
}

std::filesystem::path SettingsFile() { return UserDir() / "settings.toml"; }
std::filesystem::path DatabaseFile() { return UserDir() / "mira.db"; }

std::filesystem::path Expand(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());

  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '~' && i == 0 && (raw.size() == 1 || raw[1] == '/')) {
      out += Home().string();
    } else if (raw[i] == '$' && i + 1 < raw.size()) {
      size_t end = i + 1;
      while (end < raw.size() &&
             (std::isalnum(static_cast<unsigned char>(raw[end])) != 0 || raw[end] == '_')) {
        ++end;
      }
      const std::string name(raw.substr(i + 1, end - i - 1));
      if (const char* value = std::getenv(name.c_str())) out += value;
      i = end - 1;
    } else {
      out += raw[i];
    }
  }
  // "~/Games/" and "~/Games" name the same root; compared as paths, they differ.
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

bool IsWithin(const std::filesystem::path& target, const std::vector<std::filesystem::path>& roots,
              bool allow_equal) {
  std::error_code ec;
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(target, ec);
  if (ec) return false;
  return std::ranges::any_of(roots, [&](const std::filesystem::path& root) {
    if (root.empty()) return false;
    std::error_code root_ec;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, root_ec);
    if (root_ec) return false;
    const auto [base_end, resolved_at] = std::ranges::mismatch(base, resolved);
    if (base_end != base.end()) return false;
    return allow_equal || resolved_at != resolved.end();
  });
}

std::vector<std::filesystem::path> ListDir(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> entries;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    entries.push_back(it->path());
  }
  return entries;
}

}  // namespace mira::paths
