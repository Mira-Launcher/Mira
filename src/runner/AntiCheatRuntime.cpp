#include "runner/AntiCheatRuntime.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>

#include "config/Config.h"
#include "config/RunnerSources.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "runner/AntiCheat.h"
#include "runner/Curl.h"
#include "runner/Exec.h"
#include "steam/SteamDetector.h"

namespace mira::runner {
namespace {

namespace fs = std::filesystem;

struct Runtime {
  const char* env;          // what Proton's Wine reads
  const char* steam_dir;    // Steam's tool's folder under steamapps/common
  const char* marker;       // a folder in a runtime, to tell a whole one
  const char* url_key;      // where Mira downloads it from
  std::string_view url;     // its default
  const char* archive_dir;  // the folder the download unpacks to
  const char* sha256;       // the default download's
};

constexpr Runtime kEac{
    .env = "PROTON_EAC_RUNTIME",
    .steam_dir = "Proton EasyAntiCheat Runtime",
    .marker = "v2",
    .url_key = "anticheat.eac_runtime_url",
    .url = config::runner_sources::kEacRuntimeUrl,
    .archive_dir = "eac_runtime",
    .sha256 = "8c711a3dd0906fdedd2fdcdedc7b33e09eff5aa22fe2b2dffcc046f02e194957"};
constexpr Runtime kBattlEye{
    .env = "PROTON_BATTLEYE_RUNTIME",
    .steam_dir = "Proton BattlEye Runtime",
    .marker = "v1",
    .url_key = "anticheat.battleye_runtime_url",
    .url = config::runner_sources::kBattlEyeRuntimeUrl,
    .archive_dir = "battleeye_runtime",
    .sha256 = "229eaedf3bc16d5bf548bc2aed94bc48c44e2038337eedacbd17c6c7244cd212"};

fs::path DownloadRoot() {
  return paths::EnvOr("XDG_DATA_HOME", paths::Home() / ".local" / "share") / "mira" /
         "anticheat-runtimes";
}

// Whether the game's folder, or its exe's, has a folder of this name (any case).
bool ShipsFolder(const model::Game& game, std::string_view name) {
  if (game.install_path.empty()) return false;
  const fs::path root = game.install_path;
  for (const fs::path& dir : {root, (root / game.exe_path).parent_path()}) {
    for (const fs::path& entry : paths::ListDir(dir)) {
      if (strings::ToLower(entry.filename().string()) == name) return true;
    }
  }
  return false;
}

bool Lists(const std::optional<AntiCheat>& listed, std::string_view name) {
  return listed && std::ranges::any_of(listed->anticheats, [&](const std::string& a) {
           return strings::ToLower(a).find(name) != std::string::npos;
         });
}

Result<void> CheckSha256(const fs::path& file, std::string_view expected) {
  Command hash;
  hash.argv = {"sha256sum", file.string()};
  const Result<ExecResult> done = RunAndWait(hash);
  if (!done || done->exit_code != 0)
    return Err("checksum_unchecked", "couldn't run sha256sum to check the download");
  if (!done->output.starts_with(expected))
    return Err("checksum_mismatch", "the downloaded anti-cheat runtime's checksum didn't match");
  return {};
}

// Steam's copy, else Mira's, downloading it the first time.
Result<fs::path> RuntimeDir(const config::Config& config, const Runtime& runtime) {
  std::error_code ec;
  if (const auto steam = steam::FindSteamRoot(config)) {
    for (const fs::path& library : steam::LibraryFolders(*steam)) {
      const fs::path dir = library / "steamapps" / "common" / runtime.steam_dir;
      if (fs::is_directory(dir / runtime.marker, ec)) return dir;
    }
  }
  const fs::path dir = DownloadRoot() / runtime.archive_dir;
  static std::mutex mutex;
  const std::lock_guard lock(mutex);
  if (fs::is_directory(dir / runtime.marker, ec)) return dir;

  const std::string url = config.GetString(runtime.url_key);
  if (url.empty()) return Err("runtime_unavailable", std::format("{} is empty", runtime.url_key));
  fs::create_directories(DownloadRoot(), ec);
  const fs::path archive = DownloadRoot() / (std::string(runtime.archive_dir) + ".tar.xz");
  log::Info("downloading {} from {}", runtime.steam_dir, url);
  if (auto got = CurlDownload(url, archive, 64 << 20); !got) return std::unexpected(got.error());
  // Only the default download's checksum is known; one set by hand is the user's call.
  if (url == runtime.url) {
    if (auto ok = CheckSha256(archive, runtime.sha256); !ok) {
      fs::remove(archive, ec);
      return std::unexpected(ok.error());
    }
  }
  const fs::path unpack = DownloadRoot() / (std::string(runtime.archive_dir) + ".part");
  fs::remove_all(unpack, ec);
  fs::create_directories(unpack, ec);
  auto extracted = Extract(archive, unpack);
  fs::remove(archive, ec);
  if (!extracted) {
    fs::remove_all(unpack, ec);
    return std::unexpected(extracted.error());
  }
  if (!fs::is_directory(unpack / runtime.archive_dir / runtime.marker, ec)) {
    fs::remove_all(unpack, ec);
    return Err("runtime_unavailable",
               std::format("the download from {} isn't {}", url, runtime.steam_dir));
  }
  fs::remove_all(dir, ec);
  std::error_code renamed;
  fs::rename(unpack / runtime.archive_dir, dir, renamed);
  fs::remove_all(unpack, ec);
  if (renamed) return Err("runtime_unavailable", renamed.message());
  return dir;
}

}  // namespace

Result<std::map<std::string, std::string>> AntiCheatRuntimeEnv(const config::Config& config,
                                                               const model::Game& game) {
  std::map<std::string, std::string> env;
  if (!config.GetBool("anticheat.runtimes") || game.runner_ref.starts_with("steam:")) return env;
  const std::optional<AntiCheat> listed = FindAntiCheat(config, game);
  const bool eac = Lists(listed, "easy") || ShipsFolder(game, "easyanticheat");
  const bool battleye = Lists(listed, "battleye") || ShipsFolder(game, "battleye");
  for (const auto& [wanted, runtime] : {std::pair{eac, &kEac}, std::pair{battleye, &kBattlEye}}) {
    if (!wanted) continue;
    auto dir = RuntimeDir(config, *runtime);
    if (!dir) return std::unexpected(dir.error());
    env[runtime->env] = dir->string();
  }
  return env;
}

}  // namespace mira::runner
