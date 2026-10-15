#include "steam/AppCache.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "core/Files.h"
#include "steam/BinaryVdf.h"

namespace mira::steam {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kPackageV27 = 0x06565527;
constexpr std::uint64_t kPackageV28 = 0x06565528;
constexpr std::uint64_t kAppV28 = 0x07564428;
constexpr std::uint64_t kAppV29 = 0x07564429;

// Apps in every package the account owns; package 0 is Steam's default and isn't owned.
std::set<std::uint32_t> OwnedAppIds(const fs::path& steam_root) {
  std::set<std::uint32_t> owned;
  const auto data = files::ReadFile(steam_root / "appcache" / "packageinfo.vdf");
  if (!data) return owned;
  const auto magic = ReadLittle(*data, 0, 4);
  if (magic != kPackageV27 && magic != kPackageV28) return owned;
  const bool v28 = magic == kPackageV28;

  size_t at = 8;
  while (true) {
    const auto package_id = ReadLittle(*data, at, 4);
    if (!package_id || *package_id == 0xFFFFFFFFu) break;
    // Past the id: a 20-byte sha1, the change number, and the pics token (v28 only).
    Reader reader{*data, at + 4 + 24 + (v28 ? 8 : 0)};
    auto package = reader.Single();
    if (!package) break;
    at = reader.at;
    if (*package_id == 0) continue;
    if (Node* appids = package->Child("appids")) {
      for (const Node& app : appids->children) {
        if (app.type == kInt) owned.insert(static_cast<std::uint32_t>(app.Int32()));
      }
    }
  }
  return owned;
}

bool IsGame(std::string_view type) {
  return std::ranges::equal(type, std::string_view("game"), [](unsigned char a, unsigned char b) {
    return std::tolower(a) == std::tolower(b);
  });
}

}  // namespace

std::vector<OwnedGame> OwnedFromAppCache(const fs::path& steam_root) {
  std::vector<OwnedGame> games;
  const std::set<std::uint32_t> owned = OwnedAppIds(steam_root);
  const auto data = files::ReadFile(steam_root / "appcache" / "appinfo.vdf");
  if (!data || owned.empty()) return games;
  const auto magic = ReadLittle(*data, 0, 4);
  if (magic != kAppV29 && magic != kAppV28) return games;
  const bool v29 = magic == kAppV29;

  // v29 keeps its keys in a table at a 64-bit offset; the entries end where the table starts.
  std::vector<std::string> keys;
  size_t end = data->size();
  if (v29) {
    const auto offset = ReadLittle(*data, 8, 8);
    if (!offset || *offset < 16 || *offset >= data->size()) return games;
    end = *offset;
    const auto count = ReadLittle(*data, end, 4);
    if (!count) return games;
    Reader table{*data, end + 4};
    for (std::uint64_t i = 0; i < *count; ++i) {
      auto key = table.CString();
      if (!key) return games;
      keys.push_back(std::move(*key));
    }
  }

  size_t at = v29 ? 16 : 8;
  while (at < end) {
    const auto appid = ReadLittle(*data, at, 4);
    if (!appid || *appid == 0) break;
    // The size counts the 60 bytes of info after it, then the app's KeyValues.
    const auto size = ReadLittle(*data, at + 4, 4);
    if (!size || *size < 60) break;
    const size_t body = at + 8;
    const size_t next = body + *size;
    if (next > end) break;
    const auto id = static_cast<std::uint32_t>(*appid);
    if (owned.contains(id)) {
      Reader reader{std::string_view(*data).substr(0, next), body + 60, v29 ? &keys : nullptr};
      auto app = reader.Single();
      Node* common = app ? app->Child("common") : nullptr;
      Node* type = common ? common->Child("type") : nullptr;
      Node* name = common ? common->Child("name") : nullptr;
      if (type && name && IsGame(type->bytes)) {
        OwnedGame game;
        game.appid = std::to_string(id);
        game.name = name->bytes;
        games.push_back(std::move(game));
      }
    }
    at = next;
  }
  std::ranges::sort(games, {}, &OwnedGame::name);
  return games;
}

}  // namespace mira::steam
