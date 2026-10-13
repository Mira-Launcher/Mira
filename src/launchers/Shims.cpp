#include "launchers/Shims.h"

#include <format>
#include <fstream>

#include "core/Paths.h"
#include "core/StoreErrors.h"
#include "runner/Curl.h"
#include "runner/Exec.h"
#include "runner/Winetricks.h"

namespace mira::launchers::shims {
namespace fs = std::filesystem;
namespace {

// Wine loads its own builtin in place of a file that carries this marker.
constexpr std::size_t kBuiltinMarkerAt = 64;
constexpr char kNativeMarker[16] = "Mira native DLL";

// The release the shims URL points at now: the address its first redirect
// names (".../download/v0.1.2/..."), or empty when offline.
std::string LatestShims(const std::string& url) {
  Command command;
  command.argv = {"curl",      "-sI", "--max-time",      "5", "-o",
                  "/dev/null", "-w",  "%{redirect_url}", url};
  const auto result = runner::RunAndWait(command);
  return result && result->exit_code == 0 ? result->output : std::string();
}

}  // namespace

std::string ReadLine(const fs::path& file) {
  std::ifstream in(file);
  std::string line;
  std::getline(in, line);
  return line;
}

Result<fs::path> Dir(const config::Config& config, const fs::path& downloads,
                     std::string* version) {
  std::error_code ec;
  if (const fs::path local = config.GetPath("launchers.shims_dir"); !local.empty()) {
    if (!fs::is_regular_file(local / "sppc.dll", ec)) {
      return Err("shims_missing", std::format("no shim DLLs in {}", local.string()),
                 "Build mira-winapp-shims there, or clear launchers.shims_dir to download them.");
    }
    return local;
  }
  const fs::path dir = paths::UserDir() / "tools" / "mira-winapp-shims";
  const fs::path marker = dir / ".release";
  const std::string url = config.GetString("launchers.shims_url");
  const std::string latest = LatestShims(url);
  const std::string have = ReadLine(marker);
  const auto find = [&dir]() -> std::optional<fs::path> {
    std::error_code walk;
    for (fs::recursive_directory_iterator it(dir, walk), end; !walk && it != end;
         it.increment(walk)) {
      if (it->path().filename() == "sppc.dll") return it->path().parent_path();
    }
    return std::nullopt;
  };
  // Keep what is here unless a different release is out (offline: keep it).
  if (const auto found = find(); found && (latest.empty() || latest == have)) {
    if (version) *version = have;
    return *found;
  }
  const fs::path archive = downloads / "mira-winapp-shims.tar.gz";
  if (auto fetched = runner::CurlDownload(url, archive); !fetched) {
    if (const auto found = find()) return *found;  // an older copy beats none
    return Err("download_failed",
               "couldn't download the Windows app shims: " + fetched.error().message,
               kConnectionHint);
  }
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (auto extracted = runner::Extract(archive, dir); !extracted)
    return std::unexpected(extracted.error());
  const auto found = find();
  if (!found) return Err("shims_missing", "the shims download has no DLLs in it");
  std::ofstream(marker) << latest << "\n";
  if (version) *version = latest;
  return *found;
}

namespace {

Result<void> CopyFile(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::remove(to, ec);  // Proton links its DLLs into the prefix; never write through the link
  fs::copy_file(from, to, ec);
  if (ec)
    return Err("copy_failed",
               std::format("couldn't copy {} to {}: {}", from.string(), to.string(), ec.message()));
  return {};
}

// A Wine install's 64-bit DLLs: lib on Arch and in Proton/Wine builds, lib64 on Fedora, the
// multiarch dir on Debian.
fs::path WineDlls(const fs::path& root) {
  std::error_code ec;
  for (const char* lib : {"lib", "lib64", "lib/x86_64-linux-gnu"}) {
    const fs::path dir = root / lib / "wine" / "x86_64-windows";
    if (fs::is_directory(dir, ec)) return dir;
  }
  return root / "lib" / "wine" / "x86_64-windows";
}

}  // namespace

Result<void> Install(const config::Config& config, const runner::RunnerRegistry& runners,
                     const model::Game& host, const fs::path& downloads, std::span<const Shim> list,
                     std::string* version) {
  const auto shims = Dir(config, downloads, version);
  if (!shims) return std::unexpected(shims.error());
  const auto wine = runner::ResolveWineBinary(runners, host);
  if (!wine) return std::unexpected(wine.error());
  const fs::path wine_dlls = WineDlls(wine->parent_path().parent_path());
  const fs::path system32 = fs::path(host.data_dir) / "drive_c" / "windows" / "system32";

  std::error_code ec;
  for (const Shim& shim : list) {
    const std::string dll = std::format("{}.dll", shim.name);
    if (!shim.wine_copy.empty()) {
      fs::path original = wine_dlls / dll;
      // A newer distro Wine has Direct2D fixes Office's start screen needs.
      if (const fs::path distro = WineDlls("/usr") / dll;
          shim.name == "d2d1" && fs::is_regular_file(distro, ec)) {
        original = distro;
      }
      const fs::path copy = system32 / std::format("{}.dll", shim.wine_copy);
      if (auto copied = CopyFile(original, copy); !copied) return copied;
      if (shim.name == "d2d1") {
        std::fstream file(copy, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(kBuiltinMarkerAt);
        file.write(kNativeMarker, sizeof kNativeMarker);
      }
    }
    if (auto copied = CopyFile(*shims / dll, system32 / dll); !copied) return copied;
  }
  return {};
}

Result<void> Override(const runner::RunnerRegistry& runners, const model::Game& host,
                      std::span<const Shim> list) {
  for (const Shim& shim : list) {
    const auto added =
        runner::RunWine(runners, host,
                        {"reg", "add", "HKCU\\Software\\Wine\\DllOverrides", "/v",
                         std::string(shim.name), "/t", "REG_SZ", "/d", "native,builtin", "/f"});
    if (!added) return std::unexpected(added.error());
    if (added->exit_code != 0) {
      return Err("regedit_failed", std::format("couldn't set the {} DLL override", shim.name));
    }
  }
  return {};
}

}  // namespace mira::launchers::shims
