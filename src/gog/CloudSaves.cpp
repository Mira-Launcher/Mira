#include "gog/CloudSaves.h"

#include <fcntl.h>
#include <unistd.h>

#include <format>

#include "achievements/Achievements.h"
#include "core/Files.h"
#include "core/Json.h"
#include "core/StoreErrors.h"
#include "gog/Achievements.h"
#include "gog/Gog.h"
#include "runner/Exec.h"

namespace mira::gog {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

// What gogdl lists for a file deleted from the cloud: the md5 of an empty gzip.
constexpr std::string_view kDeletedMd5 = "aadd86936a80ee8a369579c3926f1b3c";
// The Galaxy version GOG's remote config is asked as, as Heroic asks.
constexpr std::string_view kComponentVersion = "2.0.45";

std::unexpected<Error> Unreachable(std::string what) {
  return Err("gog_api_error", std::format("GOG didn't answer with {}.", what), kConnectionHint);
}

// GETs `url` into a private temp file and returns its body. `token`, when given, goes in a
// header through curl's config file, so it stays off the process list.
Result<std::string> Fetch(const std::string& url, const std::string& token = std::string()) {
  if (!achievements::FetchableUrl(url)) return Unreachable("a usable address");
  std::string path = (fs::temp_directory_path() / "mira-gog-XXXXXX").string();
  const int fd = mkostemp(path.data(), O_CLOEXEC);
  if (fd < 0) return Unreachable("anything (no temp file)");
  close(fd);
  const std::string secrets =
      token.empty() ? std::string()
                    : runner::CurlConfigLine("header", "Authorization: Bearer " + token);
  const auto run = runner::RunCurlWithSecrets(
      {"-sSLf", "--max-time", "20", "-H", "Accept: application/json", "-o", path, url}, secrets);
  auto body = files::ReadFile(path);
  unlink(path.c_str());
  if (!run || run->exit_code != 0 || !body) return Unreachable(url);
  return std::move(*body);
}

// A build manifest's JSON: zlib-compressed as GOG serves it, or plain. Inflated with the python3
// gogdl itself runs on, since mirad doesn't link zlib.
Result<json> ManifestJson(const std::string& body) {
  json parsed = json::parse(body, nullptr, false);
  if (parsed.is_object()) return parsed;
  std::string path = (fs::temp_directory_path() / "mira-gog-meta-XXXXXX").string();
  const int fd = mkostemp(path.data(), O_CLOEXEC);
  if (fd < 0) return Unreachable("a readable build");
  const bool written = write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
  close(fd);
  Command command;
  command.argv = {"python3", "-c",
                  "import sys, zlib\n"
                  "sys.stdout.buffer.write(zlib.decompress(open(sys.argv[1], 'rb').read(), 15))",
                  path};
  const auto run = written ? runner::RunAndWait(command) : Result<runner::ExecResult>();
  unlink(path.c_str());
  if (!written || !run || run->exit_code != 0) return Unreachable("a readable build");
  parsed = json::parse(run->output, nullptr, false);
  if (!parsed.is_object()) return Unreachable("a readable build");
  return parsed;
}

// The game's own client: its id and secret, from its newest Windows build.
Result<std::pair<std::string, std::string>> ClientOf(const std::string& product_id) {
  const auto builds = Fetch(std::format(
      "{}/products/{}/os/windows/builds?generation=2",
      achievements::Endpoint("GOG_CONTENT", "https://content-system.gog.com"), product_id));
  if (!builds) return std::unexpected(builds.error());
  const json listed = json::parse(*builds, nullptr, false);
  const json items = core::JsonArray(listed, "items");
  if (items.empty()) return Unreachable("the game's builds");
  const auto meta = Fetch(core::JsonString(items[0], "link"));
  if (!meta) return std::unexpected(meta.error());
  const auto manifest = ManifestJson(*meta);
  if (!manifest) return std::unexpected(manifest.error());
  const std::string id = core::JsonString(*manifest, "clientId");
  const std::string secret = core::JsonString(*manifest, "clientSecret");
  if (id.empty() || secret.empty()) return Unreachable("the game's client");
  return std::pair{id, secret};
}

// A token for the game's own client, from gogdl, which refreshes the account's sign-in for it.
// Its output holds the token, so neither the output nor the secret goes in an error or a log.
Result<std::pair<std::string, std::string>> TokenFor(const config::Config& config,
                                                     const saves::CloudInfo& info) {
  if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
  const auto output = RunGogdl(
      config, {"auth", "--client-id", info.client_id, "--client-secret", info.client_secret});
  const json credentials = output ? core::ParseJsonTail(*output) : json();
  const std::string token = core::JsonString(credentials, "access_token");
  if (token.empty())
    return Err("not_authenticated", "GOG didn't give Mira a token for this game's cloud saves.",
               "Sign in to GOG again.", Fix::Source("gog", "login"));
  std::string user = core::JsonString(credentials, "user_id");
  if (user.empty()) user = UserId(config);
  return std::pair{token, user};
}

class GogCloud final : public saves::CloudSide {
 public:
  std::string_view Name() const override { return "GOG"; }

  Result<saves::CloudInfo> Info(const config::Config& config,
                                const model::Game& game) const override {
    if (game.platform != model::Platform::Windows)
      return saves::CloudInfo{.reason = "GOG keeps cloud saves only for Windows and Mac games."};
    if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
    const auto client = ClientOf(game.source_ref);
    if (!client) return std::unexpected(client.error());
    const auto remote = Fetch(
        std::format("{}/components/galaxy_client/clients/{}?component_version={}",
                    achievements::Endpoint("GOG_REMOTE_CONFIG", "https://remote-config.gog.com"),
                    client->first, kComponentVersion));
    if (!remote) return std::unexpected(remote.error());
    const json config_json = json::parse(*remote, nullptr, false);
    const json storage = core::JsonObject(
        core::JsonObject(core::JsonObject(config_json, "content"), "Windows"), "cloudStorage");
    saves::CloudInfo info{.client_id = client->first, .client_secret = client->second};
    if (!core::JsonBool(storage, "enabled")) {
      info.reason = "GOG doesn't keep this game's saves in its cloud.";
      return info;
    }
    info.supported = true;
    for (const json& location : core::JsonArray(storage, "locations")) {
      const std::string name = core::JsonString(location, "name");
      const std::string pattern = core::JsonString(location, "location");
      if (!name.empty() && !pattern.empty()) info.locations.push_back({name, pattern});
    }
    // Galaxy's own folder when the game names none.
    if (info.locations.empty())
      info.locations.push_back(
          {"__default", std::format("<?APPLICATION_DATA_LOCAL?>/GOG.com/Galaxy/Applications/{}/"
                                    "Storage/Shared/Files",
                                    info.client_id)});
    return info;
  }

  Result<saves::CloudListing> List(const config::Config& config, const model::Game&,
                                   const saves::CloudInfo& info,
                                   const saves::CloudLocation& location) const override {
    const auto token = TokenFor(config, info);
    if (!token) return std::unexpected(token.error());
    const auto body =
        Fetch(std::format("{}/v1/{}/{}",
                          achievements::Endpoint("GOG_CLOUD", "https://cloudstorage.gog.com"),
                          token->second, info.client_id),
              token->first);
    if (!body) return std::unexpected(body.error());
    const json listed = json::parse(*body, nullptr, false);
    if (!listed.is_array()) return Unreachable("the cloud's files");
    saves::CloudListing listing;
    const std::string prefix = location.name + "/";
    for (const json& object : listed) {
      const std::string name = core::JsonString(object, "name");
      const std::string md5 = core::JsonString(object, "hash");
      if (!name.starts_with(prefix) || md5 == kDeletedMd5) continue;
      ++listing.count;
      listing.md5[name.substr(prefix.size())] = md5;
      listing.newest_ns = std::max(
          listing.newest_ns,
          achievements::ParseTime(core::JsonString(object, "last_modified")) * 1'000'000'000);
    }
    return listing;
  }

  Result<void> Move(const config::Config& config, const model::Game& game, const saves::CloudInfo&,
                    const saves::CloudLocation& location, const fs::path& folder,
                    saves::Direction direction) const override {
    if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
    const bool down = direction == saves::Direction::Download;
    const auto output = RunGogdl(
        config, {"save-sync", folder.string(), game.source_ref, "--os", "windows", "--ts", "0",
                 "--name", location.name, down ? "--force-download" : "--force-upload"});
    // gogdl exits 0 after a failed transfer, only saying so in its log.
    if (!output || output->find("There was an error uploading a file") != std::string::npos ||
        output->find("Downloading file failed") != std::string::npos)
      return Err("cloud_sync_failed",
                 std::format("Couldn't {} {}'s saves {} GOG's cloud.", down ? "download" : "upload",
                             game.name, down ? "from" : "to"),
                 kConnectionHint);
    return {};
  }
};

}  // namespace

const saves::CloudSide& Cloud() {
  static const GogCloud cloud;
  return cloud;
}

}  // namespace mira::gog
