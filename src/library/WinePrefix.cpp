#include "library/WinePrefix.h"

#include <cstdlib>
#include <fstream>

#include "core/Strings.h"

namespace mira::library {

std::map<std::string, RegValues> ReadRegSubkeys(const std::filesystem::path& reg_file,
                                                std::string_view parent) {
  // .reg files escape backslashes and quotes inside keys and strings.
  const auto unescape = [](std::string_view in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '\\' && i + 1 < in.size()) ++i;
      out.push_back(in[i]);
    }
    return out;
  };
  const std::string prefix = strings::ToLower(parent) + "\\";

  std::map<std::string, RegValues> subkeys;
  std::ifstream in(reg_file);
  std::string line;
  std::string current;
  while (std::getline(in, line)) {
    if (line.starts_with('[')) {
      current.clear();
      const std::size_t close = line.find(']');
      if (close == std::string::npos) continue;
      const std::string key = unescape(std::string_view(line).substr(1, close - 1));
      if (!strings::ToLower(key).starts_with(prefix)) continue;
      const std::string sub = key.substr(prefix.size());
      if (!sub.empty() && sub.find('\\') == std::string::npos) current = sub;
      continue;
    }
    if (current.empty() || !line.starts_with('"')) continue;
    std::size_t end = 1;
    while (end < line.size() && line[end] != '"') end += line[end] == '\\' ? 2 : 1;
    if (end + 1 >= line.size() || line[end + 1] != '=') continue;
    const std::string name = unescape(std::string_view(line).substr(1, end - 1));
    std::string_view value = std::string_view(line).substr(end + 2);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    subkeys[current][name] = unescape(value);
  }
  return subkeys;
}

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
