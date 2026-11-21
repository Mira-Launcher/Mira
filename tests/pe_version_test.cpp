#include <doctest.h>

#include <string>

#include "library/PeVersion.h"

using mira::library::VersionString;

namespace {

// A VS_VERSIONINFO String: lengths and type, the UTF-16 key, padding to 4, the UTF-16 value.
std::string Entry(const std::string& key, const std::string& value) {
  const auto wide = [](const std::string& text) {
    std::string out;
    for (char c : text) out += {c, '\0'};
    return out + std::string(2, '\0');
  };
  std::string out(6, '\0');
  out += wide(key);
  while (out.size() % 4 != 0) out += '\0';
  out += wide(value);
  while (out.size() % 4 != 0) out += '\0';
  return out;
}

}  // namespace

TEST_CASE("A program's name is read from its version resource's strings") {
  const std::string block = Entry("CompanyName", "GOG.com") + Entry("ProductName", "Hollow Knight");
  CHECK(VersionString(block, "ProductName") == "Hollow Knight");
  CHECK(VersionString(block, "CompanyName") == "GOG.com");
  CHECK(VersionString(block, "FileDescription").empty());
}
