#include "steam/AppCache.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "core/Files.h"
#include "core/Strings.h"
#include "steam/BinaryVdf.h"
#include "steam/SteamDetector.h"
#include "steam/Vdf.h"

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

// Calls `visit` with each app in appinfo.vdf that `wanted` accepts, parsed.
void ForEachApp(const fs::path& steam_root, const std::function<bool(std::uint32_t)>& wanted,
                const std::function<void(std::uint32_t, Node&)>& visit) {
  const auto data = files::ReadFile(steam_root / "appcache" / "appinfo.vdf");
  if (!data) return;
  const auto magic = ReadLittle(*data, 0, 4);
  if (magic != kAppV29 && magic != kAppV28) return;
  const bool v29 = magic == kAppV29;

  // v29 keeps its keys in a table at a 64-bit offset; the entries end where the table starts.
  std::vector<std::string> keys;
  size_t end = data->size();
  if (v29) {
    const auto offset = ReadLittle(*data, 8, 8);
    if (!offset || *offset < 16 || *offset >= data->size()) return;
    end = *offset;
    const auto count = ReadLittle(*data, end, 4);
    if (!count) return;
    Reader table{*data, end + 4};
    for (std::uint64_t i = 0; i < *count; ++i) {
      auto key = table.CString();
      if (!key) return;
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
    if (wanted(id)) {
      Reader reader{std::string_view(*data).substr(0, next), body + 60, v29 ? &keys : nullptr};
      if (auto app = reader.Single()) visit(id, *app);
    }
    at = next;
  }
}

// A size written as text (above 2 GiB) or as a number.
std::int64_t SizeOf(const Node* node) {
  if (node == nullptr) return -1;
  if (node->type == kString) {
    try {
      return static_cast<std::int64_t>(std::stoull(node->bytes));
    } catch (...) {
      return -1;
    }
  }
  if (node->type == kInt) return static_cast<std::uint32_t>(node->Int32());
  if (node->type == kUint64) {
    if (const auto value = ReadLittle(node->bytes, 0, 8)) return static_cast<std::int64_t>(*value);
  }
  return -1;
}

std::string Text(Node* node, std::string_view key) {
  Node* child = node != nullptr ? node->Child(key) : nullptr;
  return child != nullptr && child->type == kString ? child->bytes : std::string();
}

}  // namespace

std::vector<OwnedGame> OwnedFromAppCache(const fs::path& steam_root) {
  std::vector<OwnedGame> games;
  const std::set<std::uint32_t> owned = OwnedAppIds(steam_root);
  if (owned.empty()) return games;
  ForEachApp(
      steam_root, [&owned](std::uint32_t id) { return owned.contains(id); },
      [&games](std::uint32_t id, Node& app) {
        Node* common = app.Child("common");
        Node* type = common ? common->Child("type") : nullptr;
        Node* name = common ? common->Child("name") : nullptr;
        if (type && name && strings::EqualsIgnoreCase(type->bytes, "game")) {
          OwnedGame game;
          game.appid = std::to_string(id);
          game.name = name->bytes;
          games.push_back(std::move(game));
        }
      });
  SortLikeSteam(games, steam_root);
  return games;
}

void SortLikeSteam(std::vector<OwnedGame>& games, const std::optional<fs::path>& steam_root) {
  std::map<std::string, std::string> keys;
  for (const OwnedGame& game : games) keys[game.appid] = strings::ToLower(game.name);
  if (steam_root) {
    ForEachApp(
        *steam_root, [&keys](std::uint32_t id) { return keys.contains(std::to_string(id)); },
        [&keys](std::uint32_t id, Node& app) {
          const std::string sort_as = Text(app.Child("common"), "sortas");
          if (!sort_as.empty()) keys[std::to_string(id)] = strings::ToLower(sort_as);
        });
  }
  std::ranges::stable_sort(games, [&keys](const OwnedGame& a, const OwnedGame& b) {
    return keys[a.appid] < keys[b.appid];
  });
}

std::optional<AppSize> AppSizeFromAppCache(const fs::path& steam_root, const std::string& appid) {
  std::optional<AppSize> found;
  const std::set<std::uint32_t> owned = OwnedAppIds(steam_root);
  ForEachApp(
      steam_root, [&appid](std::uint32_t id) { return std::to_string(id) == appid; },
      [&found, &owned](std::uint32_t, Node& app) {
        Node* depots = app.Child("depots");
        if (depots == nullptr) return;
        struct Depot {
          std::string os;
          std::string arch;
          std::int64_t size;
          std::int64_t download;
        };
        std::vector<Depot> candidates;
        for (Node& depot : depots->children) {
          if (depot.type != kObject || depot.children.empty()) continue;
          // Pulled from another app, shared with other games (redistributables), or DLC the
          // account doesn't own.
          if (depot.Child("depotfromapp") || depot.Child("sharedinstall") ||
              depot.Child("optionaldlc"))
            continue;
          if (Node* dlc = depot.Child("dlcappid");
              dlc != nullptr && !owned.contains(static_cast<std::uint32_t>(dlc->Int32())))
            continue;
          Node* config = depot.Child("config");
          const std::string language = Text(config, "language");
          if (!language.empty() && !strings::EqualsIgnoreCase(language, "english")) continue;
          Node* manifests = depot.Child("manifests");
          Node* manifest = manifests ? manifests->Child("public") : nullptr;
          if (manifest == nullptr) continue;
          Node* arch = config ? config->Child("osarch") : nullptr;
          candidates.push_back({strings::ToLower(Text(config, "oslist")),
                                arch != nullptr && arch->type == kString ? arch->bytes
                                : arch != nullptr ? std::to_string(arch->Int32())
                                                  : std::string(),
                                SizeOf(manifest->Child("size")),
                                SizeOf(manifest->Child("download"))});
        }
        // Steam installs a game's Linux build when it has one.
        const bool linux_build = std::ranges::any_of(
            candidates, [](const Depot& d) { return d.os.find("linux") != std::string::npos; });
        const std::string os = linux_build ? "linux" : "windows";
        const auto for_os = [&os](const Depot& d) {
          return d.os.empty() || d.os.find(os) != std::string::npos;
        };
        const bool has_64 = std::ranges::any_of(
            candidates, [&](const Depot& d) { return for_os(d) && d.arch == "64"; });
        AppSize size;
        for (const Depot& depot : candidates) {
          if (!for_os(depot) || (has_64 && depot.arch == "32")) continue;
          if (depot.size >= 0)
            size.install_bytes = std::max<std::int64_t>(size.install_bytes, 0) + depot.size;
          if (depot.download >= 0)
            size.download_bytes = std::max<std::int64_t>(size.download_bytes, 0) + depot.download;
        }
        if (size.install_bytes >= 0 || size.download_bytes >= 0) found = size;
      });
  return found;
}

namespace {

// Valve's Steam Play config: the compat tool each mapped app runs with, and each tool's appid by
// its name and its aliases ("proton-stable").
struct SteamPlayConfig {
  fs::file_time_type read_at{};
  std::map<std::string, std::string> tool_by_app;
  std::map<std::string, std::uint32_t> tool_appids;
};

SteamPlayConfig ReadSteamPlayConfig(const fs::path& steam_root) {
  SteamPlayConfig config;
  ForEachApp(
      steam_root, [](std::uint32_t id) { return id == 891390; },
      [&config](std::uint32_t, Node& app) {
        Node* extended = app.Child("extended");
        if (extended == nullptr) return;
        if (Node* tools = extended->Child("compat_tools")) {
          for (Node& tool : tools->children) {
            Node* appid = tool.Child("appid");
            if (appid == nullptr || appid->type != kInt) continue;
            const auto id = static_cast<std::uint32_t>(appid->Int32());
            config.tool_appids[tool.name] = id;
            for (const std::string& alias : strings::Split(Text(&tool, "aliases"), ','))
              config.tool_appids.try_emplace(strings::Trim(alias), id);
          }
        }
        if (Node* mappings = extended->Child("app_mappings")) {
          for (Node& mapping : mappings->children) {
            const std::string tool = Text(&mapping, "tool");
            if (!tool.empty()) config.tool_by_app[mapping.name] = tool;
          }
        }
      });
  return config;
}

}  // namespace

std::optional<fs::path> ValveProtonFor(const fs::path& steam_root, const std::string& appid) {
  // appinfo.vdf runs to hundreds of MB on a big account, so it's read once per change.
  static std::mutex mutex;
  static std::map<fs::path, SteamPlayConfig> cache;
  std::error_code ec;
  const auto modified = fs::last_write_time(steam_root / "appcache" / "appinfo.vdf", ec);
  if (ec) return std::nullopt;
  std::uint32_t tool_appid = 0;
  {
    const std::lock_guard lock(mutex);
    SteamPlayConfig& config = cache[steam_root];
    if (config.read_at != modified) {
      config = ReadSteamPlayConfig(steam_root);
      config.read_at = modified;
    }
    const auto tool = config.tool_by_app.find(appid);
    if (tool == config.tool_by_app.end()) return std::nullopt;
    const auto id = config.tool_appids.find(tool->second);
    if (id == config.tool_appids.end()) return std::nullopt;
    tool_appid = id->second;
  }
  for (const fs::path& library : LibraryFolders(steam_root)) {
    const fs::path steamapps = library / "steamapps";
    const auto text = files::ReadFile(steamapps / std::format("appmanifest_{}.acf", tool_appid));
    if (!text) continue;
    const auto parsed = ParseVdf(*text);
    const VdfValue* dir = parsed ? parsed->Get({"appstate", "installdir"}) : nullptr;
    if (dir == nullptr || !dir->scalar) continue;
    const fs::path folder = steamapps / "common" / *dir->scalar;
    if (fs::exists(folder / "proton", ec)) return folder;
  }
  return std::nullopt;
}

}  // namespace mira::steam
