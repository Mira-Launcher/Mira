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

namespace {

// appinfo.vdf (v28, inline keys) holding these apps' KeyValues bodies.
void WriteAppInfo(const fs::path& root, const std::vector<std::pair<int, std::string>>& apps) {
  std::string data = Le(0x07564428, 4) + Le(1, 4);
  for (const auto& [id, inner] : apps) {
    const std::string body = Key('\0', "appinfo") + inner + kEnd + kEnd;
    data += Le(id, 4) + Le(60 + body.size(), 4) + std::string(60, '\0') + body;
  }
  data += Le(0, 4);
  test::Touch(root / "appcache" / "appinfo.vdf", data);
}

std::string Common(const std::string& name, const std::string& sort_as = {}) {
  return Key('\0', "common") + TextField("name", name) + TextField("type", "game") +
         (sort_as.empty() ? std::string() : TextField("sortas", sort_as)) + kEnd;
}

// A depot with its config fields and its public manifest's size (as text, like Steam writes
// sizes past 2 GiB) and download (as a number).
std::string Depot(const std::string& id, const std::string& config, const std::string& size,
                  std::uint64_t download, const std::string& extra = {}) {
  return Key('\0', id) + Key('\0', "config") + config + kEnd + Key('\0', "manifests") +
         Key('\0', "public") + TextField("size", size) + IntField("download", download) + kEnd +
         kEnd + extra + kEnd;
}

}  // namespace

TEST_CASE("Steam's owned games come in its library's order") {
  std::string packages = Le(0x06565528, 4) + Le(1, 4);
  packages += Le(5, 4) + std::string(24, '\0') + Le(0, 8) + Key('\0', "5") + Key('\0', "appids") +
              IntField("0", 1) + IntField("1", 2) + IntField("2", 3) + IntField("3", 4) + kEnd +
              kEnd + kEnd;
  packages += Le(0xFFFFFFFF, 4);
  const fs::path root = test::TempDir("steam-app-order");
  test::Touch(root / "appcache" / "packageinfo.vdf", packages);
  WriteAppInfo(root, {{1, Common("Hollow Knight")},
                      {2, Common("celeste")},
                      {3, Common("Balatro")},
                      {4, Common("The Binding of Isaac", "Binding of Isaac")}});

  std::vector<std::string> names;
  for (const steam::OwnedGame& game : steam::OwnedFromAppCache(root)) names.push_back(game.name);
  CHECK(names ==
        std::vector<std::string>{"Balatro", "The Binding of Isaac", "celeste", "Hollow Knight"});

  // The Web API's list, by appid, gets the same order.
  std::vector<steam::OwnedGame> listed = {{"1", "Hollow Knight"}, {"4", "The Binding of Isaac"}};
  steam::SortLikeSteam(listed, root);
  CHECK(listed[0].name == "The Binding of Isaac");
}

TEST_CASE("A Steam title's size comes from the depots Steam would install") {
  const fs::path root = test::TempDir("steam-app-size");
  const std::string linux_game =
      Key('\0', "depots") + TextField("baselanguages", "english") +
      Depot("101", TextField("oslist", "windows"), "5000000000", 1000) +
      Depot("102", TextField("oslist", "linux"), "6000000000", 2000) +
      Depot("103", "", "10", 5) +  // every platform's
      Depot("104", TextField("oslist", "linux") + TextField("language", "german"), "999", 999) +
      Depot("105", TextField("oslist", "linux"), "777", 777,
            IntField("sharedinstall", 1) + IntField("depotfromapp", 228980)) +
      Depot("106", "", "555", 555, IntField("dlcappid", 107)) +  // DLC not owned
      Depot("107", TextField("oslist", "linux"), "1000", 1, IntField("dlcappid", 108)) + kEnd;
  // The account owns DLC 108.
  test::Touch(root / "appcache" / "packageinfo.vdf",
              Le(0x06565528, 4) + Le(1, 4) + Le(5, 4) + std::string(24, '\0') + Le(0, 8) +
                  Key('\0', "5") + Key('\0', "appids") + IntField("0", 108) + kEnd + kEnd + kEnd +
                  Le(0xFFFFFFFF, 4));
  const std::string windows_game =
      Key('\0', "depots") +
      Depot("201", TextField("oslist", "windows") + IntField("osarch", 32), "100", 50) +
      Depot("202", TextField("oslist", "windows") + IntField("osarch", 64), "200", 80) + kEnd;
  WriteAppInfo(root, {{100, Common("Hollow Knight") + linux_game},
                      {200, Common("Satisfactory") + windows_game},
                      {300, Common("Balatro")}});

  const auto linux_size = steam::AppSizeFromAppCache(root, "100");
  REQUIRE(linux_size.has_value());
  CHECK(linux_size->install_bytes == 6000001010);
  CHECK(linux_size->download_bytes == 2006);

  const auto windows_size = steam::AppSizeFromAppCache(root, "200");
  REQUIRE(windows_size.has_value());
  CHECK(windows_size->install_bytes == 200);
  CHECK(windows_size->download_bytes == 80);

  CHECK_FALSE(steam::AppSizeFromAppCache(root, "300").has_value());
  CHECK_FALSE(steam::AppSizeFromAppCache(root, "400").has_value());
}
