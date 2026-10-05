#include "core/Strings.h"
#include "itch/Itch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <ranges>
#include <regex>
#include <utility>

#include <json.hpp>

#include "itch/Butlerd.h"
#include "core/StoreErrors.h"
#include "runner/Exec.h"

namespace mira::itch {
namespace {
namespace fs = std::filesystem;
}  // namespace

std::filesystem::path ManagedButlerPath(const config::Config& config) {
  // InstallButlerBinary extracts butler's own zip as-is (it ships nested
  // under "linux-amd64/", alongside .so files it needs there) rather than
  // flattening it -- so this has to search for the binary, not assume a
  // flat "tools/itch/butler" layout.
  const fs::path tools_dir = config.File().parent_path() / "tools" / "itch";
  std::error_code ec;
  for (const auto& entry : fs::recursive_directory_iterator(tools_dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().filename() == "butler") return entry.path();
  }
  return tools_dir / "butler";  // sensible default even if not installed yet
}

const runner::StoreTool kTool = {"itch", "itch.io", "butler", "itch.butler_bin", ManagedButlerPath};

runner::ToolStatus DetectButler(const config::Config& config) { return runner::DetectTool(config, kTool); }

Result<void> InstallButlerBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  auto installed = runner::InstallToolBinary(config, "itch", asset, "butler");
  if (!installed) return std::unexpected(installed.error());
  return {};
}

std::filesystem::path ApiKeyFile(const config::Config& config) {
  return config.File().parent_path() / "itch-api-key";
}

runner::AuthStatus Status(const config::Config& config) {
  runner::AuthStatus status;
  status.tool = DetectButler(config);
  if (!status.tool.installed) return status;

  std::error_code ec;
  status.authenticated = fs::exists(ApiKeyFile(config), ec) && fs::file_size(ApiKeyFile(config), ec) > 0;
  return status;
}

Result<void> CheckReady(const config::Config& config) {
  return runner::CheckStoreReady(kTool, Status(config));
}

Result<void> Login(const config::Config& config, const std::string& pasted_key) {
  const std::string api_key = strings::Trim(pasted_key);
  // Checked before anything is written, so a bad key can't log out a working one.
  if (const Result<nlohmann::json> result = Call(config, "Profile.LoginWithAPIKey", {{"apiKey", api_key}}); !result) {
    return std::unexpected(result.error());
  }
  const fs::path key_file = ApiKeyFile(config);
  std::error_code ec;
  fs::create_directories(key_file.parent_path(), ec);
  // Owner-only before the key is written, not after.
  const int fd = ::open(key_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return Err("write_failed", "couldn't write " + key_file.string());
  fchmod(fd, 0600);
  const bool written = ::write(fd, api_key.data(), api_key.size()) == static_cast<ssize_t>(api_key.size());
  ::close(fd);
  if (!written) return Err("write_failed", "couldn't write " + key_file.string());
  return {};
}

Result<void> Logout(const config::Config& config) {
  std::error_code ec;
  fs::remove(ApiKeyFile(config), ec);
  return {};
}

Result<std::int64_t> CurrentProfileId(const config::Config& config) {
  std::ifstream file(ApiKeyFile(config));
  if (!file) return StoreNotSignedIn("itch", "itch.io");
  const std::string api_key((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  // No separate persisted profile id (see this function's header comment)
  // -- re-authenticating with the stored key is how butlerd hands it back.
  // Remembered per key, so one listing or install logs in once.
  static std::mutex cache_mutex;
  static std::pair<std::string, std::int64_t> cached;
  {
    const std::lock_guard lock(cache_mutex);
    if (cached.first == api_key) return cached.second;
  }
  const Result<nlohmann::json> result = Call(config, "Profile.LoginWithAPIKey", {{"apiKey", api_key}});
  if (!result) return std::unexpected(result.error());
  if (!result->contains("profile") || !(*result)["profile"].contains("id") ||
      !(*result)["profile"]["id"].is_number_integer()) {
    return Err("itch_profile_missing", "butlerd didn't return a profile id");
  }
  const std::int64_t id = (*result)["profile"]["id"].get<std::int64_t>();
  const std::lock_guard lock(cache_mutex);
  cached = {api_key, id};
  return id;
}

Result<void> EnsureInstallLocation(const config::Config& config) {
  const fs::path path = config.GetPath("itch.install_root");
  std::error_code ec;
  fs::create_directories(path, ec);  // butlerd expects it to already exist
  if (ec) return Err("install_dir_failed", ec.message());

  const Result<nlohmann::json> result =
    Call(config, "Install.Locations.Add", {{"id", "mira"}, {"path", path.string()}});
  if (!result) return std::unexpected(result.error());
  return {};
}

namespace {

ItchCollection FromButler(const nlohmann::json& collection, bool own) {
  return {.id = collection.value("id", std::int64_t{0}),
          .title = collection.value("title", std::string()),
          .games_count = collection.value("gamesCount", std::int64_t{0}),
          .own = own};
}

Result<ItchCollection> FetchCollection(const config::Config& config, std::int64_t profile_id, std::int64_t id) {
  const Result<nlohmann::json> fetched =
    Call(config, "Fetch.Collection", {{"profileId", profile_id}, {"collectionId", id}, {"fresh", true}});
  if (!fetched || !fetched->contains("collection") || !(*fetched)["collection"].is_object()) {
    return Err("collection_unreadable",
               "couldn't read that collection. Check the link, or it may be another account's private collection");
  }
  return FromButler((*fetched)["collection"], false);
}

}  // namespace

std::optional<std::int64_t> ParseCollectionLink(std::string_view link) {
  static const std::regex kLink(R"(^\s*(?:(?:https?://)?(?:www\.)?itch\.io/c/)?(\d{1,18})(?:[/?#].*)?\s*$)");
  std::cmatch match;
  if (!std::regex_match(link.data(), link.data() + link.size(), match, kLink)) return std::nullopt;
  const std::int64_t id = std::stoll(match[1].str());
  return id > 0 ? std::optional(id) : std::nullopt;
}

Result<std::vector<ItchCollection>> ListCollections(const config::Config& config) {
  const Result<std::int64_t> profile_id = CurrentProfileId(config);
  if (!profile_id) return std::unexpected(profile_id.error());

  std::vector<ItchCollection> collections;
  const Result<nlohmann::json> own =
    Call(config, "Fetch.ProfileCollections", {{"profileId", *profile_id}, {"fresh", true}});
  if (!own) return std::unexpected(own.error());
  for (const nlohmann::json& item : own->value("items", nlohmann::json::array())) {
    collections.push_back(FromButler(item, true));
  }
  for (const std::string& added : config.GetStringArray("itch.collections")) {
    const std::optional<std::int64_t> id = ParseCollectionLink(added);
    if (!id || std::ranges::contains(collections, *id, &ItchCollection::id)) continue;
    if (const Result<ItchCollection> collection = FetchCollection(config, *profile_id, *id)) {
      collections.push_back(*collection);
    }
  }
  return collections;
}

Result<ItchCollection> AddCollection(config::Config& config, std::string_view link) {
  const std::optional<std::int64_t> id = ParseCollectionLink(link);
  if (!id) return Err("invalid_link", "expected a collection link like https://itch.io/c/123/name");
  const Result<std::int64_t> profile_id = CurrentProfileId(config);
  if (!profile_id) return std::unexpected(profile_id.error());
  // Already listed as one of your own: nothing to save.
  if (const Result<nlohmann::json> own =
        Call(config, "Fetch.ProfileCollections", {{"profileId", *profile_id}, {"fresh", true}})) {
    for (const nlohmann::json& item : own->value("items", nlohmann::json::array())) {
      if (item.value("id", std::int64_t{0}) == *id) return FromButler(item, true);
    }
  }
  const Result<ItchCollection> collection = FetchCollection(config, *profile_id, *id);
  if (!collection) return collection;

  std::vector<std::string> added = config.GetStringArray("itch.collections");
  const std::string key = std::to_string(*id);
  if (!std::ranges::contains(added, key)) {
    added.push_back(key);
    if (auto saved = config.Set("itch.collections", added); !saved) return std::unexpected(saved.error());
  }
  return collection;
}

Result<void> RemoveCollection(config::Config& config, std::int64_t id) {
  std::vector<std::string> added = config.GetStringArray("itch.collections");
  const auto removed = std::ranges::remove_if(
    added, [id](const std::string& entry) { return ParseCollectionLink(entry) == std::optional(id); });
  if (removed.begin() == added.end()) {
    return Err("not_found", "that collection wasn't added by link (your own collections can't be removed)");
  }
  added.erase(removed.begin(), removed.end());
  return config.Set("itch.collections", added);
}

}  // namespace mira::itch
