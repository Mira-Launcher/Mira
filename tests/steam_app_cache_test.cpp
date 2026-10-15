#include <doctest.h>

#include <cstdint>
#include <string>

#include "steam/AppCache.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

std::string Le(std::uint64_t value, int width) {
  std::string bytes;
  for (int i = 0; i < width; ++i) bytes += char((value >> (8 * i)) & 0xff);
  return bytes;
}

// A type byte and a NUL-terminated name.
std::string Key(char type, const std::string& name) {
  return std::string(1, type) + name + std::string(1, '\0');
}

std::string TextField(const std::string& name, const std::string& text) {
  return Key('\x01', name) + text + std::string(1, '\0');
}

std::string IntField(const std::string& name, std::uint64_t value) {
  return Key('\x02', name) + Le(value, 4);
}

const std::string kEnd(1, '\x08');

}  // namespace

TEST_CASE("Owned games come from Steam's app cache when there's no Web API key") {
  // Each KeyValues block ends with one more end byte, as in Steam's files.
  // Package 0 is Steam's default and owns nothing; package 5 owns apps 20 and 30.
  std::string packages = Le(0x06565528, 4) + Le(1, 4);
  auto package = [&](std::uint64_t id, const std::string& body) {
    packages += Le(id, 4) + std::string(24, '\0') + Le(0, 8) + body + kEnd;
  };
  package(0, Key('\0', "0") + Key('\0', "appids") + IntField("0", 10) + kEnd + kEnd);
  package(5, Key('\0', "5") + Key('\0', "appids") + IntField("0", 20) + IntField("1", 30) + kEnd +
                 kEnd);
  packages += Le(0xFFFFFFFF, 4);

  std::string apps = Le(0x07564428, 4) + Le(1, 4);
  auto app = [&](std::uint64_t id, const std::string& type, const std::string& name) {
    const std::string body = Key('\0', "appinfo") + Key('\0', "common") + TextField("name", name) +
                             TextField("type", type) + kEnd + kEnd + kEnd;
    apps += Le(id, 4) + Le(60 + body.size(), 4) + std::string(60, '\0') + body;
  };
  app(10, "game", "Free");
  app(20, "Game", "Owned");
  app(30, "Tool", "Tool");
  apps += Le(0, 4);

  const fs::path root = test::TempDir("steam-app-cache");
  test::Touch(root / "appcache" / "packageinfo.vdf", packages);
  test::Touch(root / "appcache" / "appinfo.vdf", apps);

  const auto owned = steam::OwnedFromAppCache(root);
  REQUIRE(owned.size() == 1);
  CHECK(owned[0].appid == "20");
  CHECK(owned[0].name == "Owned");
}
