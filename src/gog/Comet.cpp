#include "gog/Comet.h"

#include <json.hpp>
#include <map>
#include <mutex>

#include "achievements/Achievements.h"
#include "core/Json.h"
#include "core/Log.h"
#include "gog/Achievements.h"
#include "gog/Gog.h"
#include "runner/Exec.h"

namespace mira::gog {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

const runner::StoreTool kComet = {"gog", "GOG", "comet", "gog.comet_bin", ManagedCometPath};

// Points comet's Heroic sign-in lookup at gogdl's file. Returns the folder for $XDG_CONFIG_PATH.
Result<fs::path> LinkSignIn(const config::Config& config) {
  const fs::path dir = config.File().parent_path() / "gog-comet";
  const fs::path link = dir / "heroic" / "gog_store" / "auth.json";
  const fs::path target = AuthConfigPath(config);
  std::error_code ec;
  if (fs::read_symlink(link, ec) == target) return dir;
  fs::create_directories(link.parent_path(), ec);
  fs::remove(link, ec);
  fs::create_symlink(target, link, ec);
  if (ec && fs::read_symlink(link, ec) != target)
    return Err("comet_failed", "couldn't point comet at GOG's sign-in: " + ec.message());
  return dir;
}

}  // namespace

std::filesystem::path ManagedCometPath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "comet" / "comet";
}

runner::ToolStatus DetectComet(const config::Config& config) {
  return runner::DetectTool(config, kComet);
}

Result<void> InstallCometBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  auto installed = runner::InstallToolBinary(config, "comet", asset, "comet");
  if (!installed) return std::unexpected(installed.error());
  return {};
}

std::string Username(const config::Config& config) {
  static std::mutex mutex;
  static std::map<std::string, std::string> known;
  const std::string user = UserId(config);
  {
    const std::lock_guard lock(mutex);
    if (const auto found = known.find(user); found != known.end()) return found->second;
  }
  std::string name;
  if (const auto token = AccessToken(config)) {
    const auto result = runner::RunCurlWithSecrets(
        {"-sSL", "-f", "--max-time", "5",
         achievements::Endpoint("GOG_EMBED", "https://embed.gog.com") + "/userData.json"},
        runner::CurlConfigLine("header", "Authorization: Bearer " + *token));
    if (result && result->exit_code == 0) {
      const json parsed = json::parse(result->output, nullptr, false);
      if (parsed.is_object()) name = core::JsonString(parsed, "username");
    }
  }
  if (name.empty()) return user;  // asked again next time
  const std::lock_guard lock(mutex);
  known[user] = name;
  return name;
}

Result<Command> CometCommand(const config::Config& config) {
  if (auto ready = CheckReady(config); !ready) return std::unexpected(ready.error());
  const runner::ToolStatus comet = DetectComet(config);
  if (!comet.installed)
    return Err("comet_missing", "comet isn't installed, so GOG achievements can't unlock",
               "Set up GOG again to download it.", Fix::Source("gog", "setup"));
  // comet reads the token once at start, so refresh a stale one first.
  if (auto token = AccessToken(config); !token) return std::unexpected(token.error());
  const auto config_dir = LinkSignIn(config);
  if (!config_dir) return std::unexpected(config_dir.error());
  Command command;
  command.argv = {comet.path, "--from-heroic", "--username", Username(config), "--quit"};
  command.env["XDG_CONFIG_PATH"] = config_dir->string();
  return command;
}

}  // namespace mira::gog
