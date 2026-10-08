#include "core/Files.h"

#include <fstream>
#include <iterator>

namespace mira::files {

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace mira::files
