#include "library/WinePrefix.h"

#include <cstdlib>

namespace mira::library {

bool LooksLikeWinePrefix(const std::filesystem::path& dir) {
  std::error_code ec;
  const bool has_registry = std::filesystem::exists(dir / "system.reg", ec);
  const bool has_drive_c = std::filesystem::exists(dir / "drive_c", ec);
  const bool has_pfx = std::filesystem::exists(dir / "pfx", ec);
  return (has_registry && has_drive_c) || has_pfx;
}

bool IsPrefixSystemDir(const std::filesystem::path& rel) {
  return rel.lexically_normal().generic_string() == "drive_c/windows";
}

std::filesystem::path PrefixRoot(const std::filesystem::path& dir) {
  std::error_code ec;
  if (std::filesystem::is_directory(dir / "pfx" / "drive_c", ec)) return dir / "pfx";
  return dir;
}

std::filesystem::path WindowsUserDir(const std::filesystem::path& prefix) {
  const std::filesystem::path users = prefix / "drive_c" / "users";
  std::error_code ec;
  if (std::filesystem::is_directory(users / "steamuser", ec)) return users / "steamuser";
  const char* user = std::getenv("USER");
  return users / (user != nullptr && *user != '\0' ? user : "user");
}

}  // namespace mira::library
