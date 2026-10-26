#include "saves/Cloud.h"

#include <algorithm>
#include <format>
#include <optional>

#include "config/Resolver.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "epic/CloudSaves.h"
#include "gog/CloudSaves.h"
#include "runner/Exec.h"

namespace mira::saves {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

// How long a store's answer about where a game's cloud saves are is used before asking again.
constexpr std::int64_t kInfoFor = 7 * 24 * 3600;
// Both sides' newest times this close count as the same saves on a first sync where the store
// gives no checksums (Epic), as legendary itself decides.
constexpr std::int64_t kSameAgeNs = 60'000'000'000;

// A glob that names one literal path: its escapes undone. Empty when it has a real wildcard.
std::string Literal(std::string_view glob) {
  std::string out;
  for (std::size_t i = 0; i < glob.size(); ++i) {
    const char c = glob[i];
    if (c == '[' && i + 2 < glob.size() && glob[i + 2] == ']' &&
        (glob[i + 1] == '[' || glob[i + 1] == '*' || glob[i + 1] == '?')) {
      out += glob[i + 1];
      i += 2;
    } else if (c == '*' || c == '?' || c == '[') {
      return {};
    } else {
      out += c;
    }
  }
  return out;
}

// `path` with each missing component replaced by an existing one of the same name in another
// case, as Windows would find it.
fs::path Folded(const fs::path& path) {
  fs::path out = "/";
  bool found = true;
  for (const fs::path& part : path.relative_path()) {
    std::error_code ec;
    if (found && !fs::exists(out / part, ec)) {
      found = false;
      const std::string lower = strings::ToLower(part.string());
      for (const fs::path& entry : paths::ListDir(out)) {
        if (strings::ToLower(entry.filename().string()) == lower) {
          out = entry;
          found = true;
          break;
        }
      }
      if (found) continue;
    }
    out /= part;
  }
  return out;
}

std::string_view Pressing(std::span<const std::string> states) {
  for (const std::string_view state : {kConflict, kCloudNewer, kLocalNewer, kUnknown})
    if (std::ranges::contains(states, state)) return state;
  return states.empty() ? kUnknown : kSynced;
}

// The local files' md5 sums by path in `folder`, through coreutils' md5sum.
std::map<std::string, std::string> LocalMd5(const fs::path& folder,
                                            const std::vector<SaveFile>& files) {
  std::map<std::string, std::string> sums;
  if (files.empty()) return sums;
  Command command;
  command.argv = {"md5sum", "--"};
  for (const SaveFile& file : files) command.argv.push_back(file.absolute.string());
  const auto run = runner::RunAndWait(command);
  if (!run || run->exit_code != 0) return sums;
  for (const std::string_view line : strings::Split(run->output, '\n')) {
    if (line.size() < 35 || line[32] != ' ') continue;
    const fs::path file(std::string(line.substr(34)));
    sums[file.lexically_relative(folder).generic_string()] = std::string(line.substr(0, 32));
  }
  return sums;
}

struct Look {
  CloudLocation location;
  fs::path folder;
  std::vector<SaveFile> files{};
  std::string marker{};
  CloudListing cloud{};
  std::optional<store::CloudSync> row{};
  std::string state{};
};

bool EverSynced(const std::optional<store::CloudSync>& row) {
  return row && row->synced_at > 0;
}

std::string Classify(const Look& look) {
  const bool local = !look.files.empty();
  const bool cloud = look.cloud.count > 0;
  if (!EverSynced(look.row)) {
    if (!local && !cloud) return std::string(kSynced);
    if (!local) return std::string(kCloudNewer);
    if (!cloud) return std::string(kLocalNewer);
    if (!look.cloud.md5.empty()) {
      return LocalMd5(look.folder, look.files) == look.cloud.md5 ? std::string(kSynced)
                                                                 : std::string(kConflict);
    }
    std::int64_t newest = 0;
    for (const SaveFile& file : look.files) newest = std::max(newest, file.mtime_ns);
    return std::llabs(newest - look.cloud.newest_ns) <= kSameAgeNs ? std::string(kSynced)
                                                                   : std::string(kConflict);
  }
  const bool local_changed = look.marker != look.row->local_marker;
  const bool cloud_changed = look.cloud.newest_ns > look.row->synced_at;
  if (local_changed && cloud_changed) return std::string(kConflict);
  if (cloud_changed) return std::string(kCloudNewer);
  if (local_changed) return std::string(kLocalNewer);
  return std::string(kSynced);
}

std::vector<SaveFile> FilesIn(const CloudLocation& location, const fs::path& folder,
                              const Roots& roots) {
  std::vector<SaveFile> files;
  if (folder.empty()) return files;
  for (SaveFile& file : Find(location.pattern, roots).files)
    if (paths::IsWithin(file.absolute, {folder})) files.push_back(std::move(file));
  return files;
}

// The game's cloud info, cached for kInfoFor; a cached answer stands in when the store can't be
// asked.
Result<CloudInfo> InfoFor(const config::Config& config, store::MetadataStore& cache,
                          const model::Game& game, const CloudSide& side) {
  const json cached = cache.ReadCloudSaves(game.id);
  if (cached.is_object() && model::NowSeconds() - core::JsonInt(cached, "fetched_at") < kInfoFor)
    return CloudInfoFromJson(cached);
  auto asked = side.Info(config, game);
  if (!asked) {
    if (cached.is_object()) return CloudInfoFromJson(cached);
    return std::unexpected(asked.error());
  }
  if (auto written = cache.WriteCloudSaves(game.id, CloudInfoJson(*asked)); !written)
    log::Warn("couldn't keep where {}'s cloud saves are: {}", game.id, written.error().message);
  return asked;
}

json LocationJson(const Look& look) {
  return {{"name", look.location.name}, {"path", look.folder.string()}, {"state", look.state}};
}

}  // namespace

json CloudInfoJson(const CloudInfo& info) {
  json locations = json::array();
  for (const CloudLocation& location : info.locations)
    locations.push_back({{"name", location.name}, {"pattern", location.pattern}});
  return {{"supported", info.supported},
          {"reason", info.reason},
          {"locations", std::move(locations)},
          {"client_id", info.client_id},
          {"client_secret", info.client_secret}};
}

CloudInfo CloudInfoFromJson(const json& value) {
  CloudInfo info{.supported = core::JsonBool(value, "supported"),
                 .reason = core::JsonString(value, "reason"),
                 .client_id = core::JsonString(value, "client_id"),
                 .client_secret = core::JsonString(value, "client_secret")};
  for (const json& location : core::JsonArray(value, "locations"))
    info.locations.push_back({.name = core::JsonString(location, "name"),
                              .pattern = core::JsonString(location, "pattern")});
  return info;
}

const CloudSide* CloudFor(const model::Game& game) {
  if (game.source == "gog") return &gog::Cloud();
  if (game.source == "epic") return &epic::Cloud();
  return nullptr;
}

std::string LocalMarker(const fs::path& folder, const std::vector<SaveFile>& files) {
  if (files.empty()) return "none";
  std::vector<std::string> lines;
  for (const SaveFile& file : files)
    lines.push_back(std::format("{}\t{}\t{}",
                                file.absolute.lexically_relative(folder).generic_string(),
                                file.bytes, file.mtime_ns));
  std::ranges::sort(lines);
  std::string text;
  for (const std::string& line : lines) text += line + "\n";
  return strings::StableHash(text);
}

fs::path CloudFolder(const CloudLocation& location, const Roots& roots) {
  if (Find(location.pattern, roots).problem == "too_broad") return {};
  fs::path first;
  for (const std::string& glob : Expand(location.pattern, roots)) {
    const std::string literal = Literal(glob);
    if (literal.empty()) continue;
    fs::path folder = fs::path(literal).lexically_normal();
    if (!folder.has_filename()) folder = folder.parent_path();  // Epic's end in '/'
    if (roots.windows) folder = Folded(folder);
    std::error_code ec;
    if (fs::is_directory(folder, ec)) return folder;
    if (first.empty()) first = folder;
  }
  return first;
}

bool KnownUnsupported(const store::MetadataStore& cache, const model::Game& game) {
  const json cached = cache.ReadCloudSaves(game.id);
  return cached.is_object() &&
         model::NowSeconds() - core::JsonInt(cached, "fetched_at") < kInfoFor &&
         !core::JsonBool(cached, "supported");
}

std::vector<std::string> CachedCloudPatterns(const store::MetadataStore& cache,
                                             const model::Game& game) {
  std::vector<std::string> patterns;
  if (CloudFor(game) == nullptr) return patterns;
  const CloudInfo info = CloudInfoFromJson(cache.ReadCloudSaves(game.id));
  if (!info.supported) return patterns;
  for (const CloudLocation& location : info.locations) patterns.push_back(location.pattern);
  return patterns;
}

json CloudOverview(const config::Config& config, const store::MetadataStore& cache,
                   const store::GameStore& games, const model::Game& game) {
  const CloudSide* side = CloudFor(game);
  if (side == nullptr) return nullptr;
  json answer = {
      {"store", side->Name()}, {"synced_at", 0}, {"checked_at", 0}, {"local_at", 0},
      {"cloud_at", 0},         {"files", 0},     {"bytes", 0},      {"locations", json::array()}};
  if (!config::Resolver(config, game.overrides).GetBool("saves.cloud")) {
    answer["state"] = "off";
    return answer;
  }
  const json cached = cache.ReadCloudSaves(game.id);
  if (!cached.is_object()) {
    answer["state"] = kUnknown;
    return answer;
  }
  const CloudInfo info = CloudInfoFromJson(cached);
  if (!info.supported) {
    answer["state"] = "unsupported";
    answer["reason"] = info.reason;
    return answer;
  }
  const Roots roots = RootsFor(config, game);
  const std::vector<store::CloudSync> rows = games.CloudSyncs(game.id);
  std::vector<std::string> states;
  for (const CloudLocation& location : info.locations) {
    Look look{.location = location, .folder = CloudFolder(location, roots)};
    const auto row = std::ranges::find(rows, location.name, &store::CloudSync::location);
    look.state = kUnknown;
    look.files = FilesIn(location, look.folder, roots);
    for (const SaveFile& file : look.files) {
      answer["local_at"] =
          std::max(answer["local_at"].get<std::int64_t>(), file.mtime_ns / 1'000'000'000);
      answer["bytes"] = answer["bytes"].get<std::uintmax_t>() + file.bytes;
    }
    answer["files"] = answer["files"].get<std::size_t>() + look.files.size();
    if (row != rows.end()) {
      look.state = row->state;
      // Played since the check: the local side has changed too.
      if (row->synced_at > 0 && LocalMarker(look.folder, look.files) != row->local_marker) {
        if (look.state == kSynced) look.state = kLocalNewer;
        if (look.state == kCloudNewer) look.state = kConflict;
      }
      answer["synced_at"] =
          std::max(answer["synced_at"].get<std::int64_t>(), row->synced_at / 1'000'000'000);
      answer["checked_at"] = std::max(answer["checked_at"].get<std::int64_t>(), row->checked_at);
      answer["cloud_at"] =
          std::max(answer["cloud_at"].get<std::int64_t>(), row->cloud_newest / 1'000'000'000);
    }
    states.push_back(look.state);
    answer["locations"].push_back(LocationJson(look));
  }
  answer["state"] = Pressing(states);
  return answer;
}

Result<json> RunCloud(const config::Config& config, store::MetadataStore& cache,
                      store::GameStore& games, const model::Game& game, CloudAction action,
                      const std::function<Result<void>()>& before_download) {
  const CloudSide* side = CloudFor(game);
  if (side == nullptr)
    return Err("cloud_unsupported", std::format("{} has no cloud saves Mira syncs.", game.name));
  const auto info = InfoFor(config, cache, game, *side);
  if (!info) return std::unexpected(info.error());
  if (!info->supported)
    return Err("cloud_unsupported", std::format("{} keeps no saves in {}'s cloud: {}", game.name,
                                                side->Name(), info->reason));

  const Roots roots = RootsFor(config, game);
  const std::vector<store::CloudSync> rows = games.CloudSyncs(game.id);
  std::vector<Look> looks;
  for (const CloudLocation& location : info->locations) {
    Look look{.location = location, .folder = CloudFolder(location, roots)};
    if (look.folder.empty())
      return Err("cloud_unsupported",
                 std::format("Mira can't tell where {}'s cloud saves \"{}\" go on this computer "
                             "({}).",
                             game.name, location.name, location.pattern));
    look.files = FilesIn(location, look.folder, roots);
    look.marker = LocalMarker(look.folder, look.files);
    if (const auto row = std::ranges::find(rows, location.name, &store::CloudSync::location);
        row != rows.end())
      look.row = *row;
    auto listing = side->List(config, game, *info, location);
    if (!listing) return std::unexpected(listing.error());
    look.cloud = std::move(*listing);
    look.state = Classify(look);
    looks.push_back(std::move(look));
  }

  const std::int64_t now = model::NowSeconds();
  const auto store_check = [&](const Look& look) {
    store::CloudSync row = look.row.value_or(store::CloudSync{.location = look.location.name});
    row.state = look.state;
    row.cloud_newest = look.cloud.newest_ns;
    row.checked_at = now;
    if (!EverSynced(look.row) && look.state == kSynced) {
      row.synced_at = std::max<std::int64_t>(look.cloud.newest_ns, 1);
      row.local_marker = look.marker;
    }
    if (auto stored = games.SetCloudSync(game.id, row); !stored)
      log::Warn("couldn't keep {}'s cloud save state: {}", game.id, stored.error().message);
  };
  for (const Look& look : looks) store_check(look);

  std::vector<std::string> states;
  for (const Look& look : looks) states.push_back(look.state);
  if (action == CloudAction::Sync && std::ranges::contains(states, kConflict))
    return Err("saves_conflict",
               std::format("{}'s saves changed both here and in {}'s cloud since they were last "
                           "in sync.",
                           game.name, side->Name()),
               "Choose which to keep. The other is backed up first, so either choice can be "
               "undone.",
               Fix::Game(game.id, "saves"));

  // Nothing is moved from an empty side: gogdl turns either direction into the other then.
  const auto direction_for = [&](const Look& look) -> std::optional<Direction> {
    if (action == CloudAction::Download)
      return look.cloud.count > 0 ? std::optional(Direction::Download) : std::nullopt;
    if (action == CloudAction::Upload)
      return !look.files.empty() ? std::optional(Direction::Upload) : std::nullopt;
    if (action == CloudAction::Check) return std::nullopt;
    if (look.state == kCloudNewer) return Direction::Download;
    if (look.state == kLocalNewer) return Direction::Upload;
    return std::nullopt;
  };
  bool backed_up = false;
  json result_locations = json::array();
  for (Look& look : looks) {
    const auto direction = direction_for(look);
    json item = LocationJson(look);
    if (direction) {
      if (*direction == Direction::Download && !backed_up) {
        if (auto saved = before_download(); !saved) return std::unexpected(saved.error());
        backed_up = true;
      }
      if (auto moved = side->Move(config, game, *info, look.location, look.folder, *direction);
          !moved)
        return std::unexpected(moved.error());
      auto listing = side->List(config, game, *info, look.location);
      if (!listing) return std::unexpected(listing.error());
      look.files = FilesIn(look.location, look.folder, roots);
      store::CloudSync row{.location = look.location.name,
                           .state = std::string(kSynced),
                           .synced_at = std::max<std::int64_t>(listing->newest_ns, 1),
                           .cloud_newest = listing->newest_ns,
                           .local_marker = LocalMarker(look.folder, look.files),
                           .checked_at = model::NowSeconds()};
      if (auto stored = games.SetCloudSync(game.id, row); !stored)
        log::Warn("couldn't keep {}'s cloud save state: {}", game.id, stored.error().message);
      look.state = kSynced;
      item["state"] = look.state;
      item["moved"] = *direction == Direction::Download ? "download" : "upload";
    }
    result_locations.push_back(std::move(item));
  }
  states.clear();
  for (const Look& look : looks) states.push_back(look.state);
  return json{{"state", Pressing(states)}, {"locations", std::move(result_locations)}};
}

}  // namespace mira::saves
