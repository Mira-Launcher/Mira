#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

void RunOrFail(const std::vector<std::string>& argv, const fs::path& cwd = {}) {
  Command command;
  command.argv = argv;
  if (!cwd.empty()) command.cwd = cwd;
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  REQUIRE(result);
  REQUIRE(result->exit_code == 0);
}

// Zips `source_dir` (kept as the archive's own top-level entry, e.g.
// "linux-amd64/...") into `zip_path`, via python3's stdlib zipfile module
// rather than depending on a `zip` CLI (not guaranteed present).
void WriteZip(const fs::path& zip_path, const fs::path& source_dir) {
  RunOrFail({"python3", "-c",
            "import zipfile, os, sys\n"
            "root = sys.argv[2]\n"
            "with zipfile.ZipFile(sys.argv[1], 'w') as zf:\n"
            "    for dirpath, _, files in os.walk(root):\n"
            "        for name in files:\n"
            "            full = os.path.join(dirpath, name)\n"
            "            zf.write(full, os.path.relpath(full, os.path.dirname(root)))\n",
            zip_path.string(), source_dir.string()});
}

}  // namespace

TEST_CASE("InstallToolBinary installs a bare binary directly, renamed to binary_name") {
  const fs::path dir = TempDir("downloader-bare-binary");
  const fs::path source = dir / "gogdl_linux_x86_64";
  std::ofstream(source) << "#!/bin/sh\necho hi\n";

  runner::ReleaseAsset asset;
  asset.tag = "v1.0.0";
  asset.asset_name = "gogdl_linux_x86_64";
  asset.download_url = "file://" + source.string();  // curl's own "download" the same as a real URL

  config::Config config(dir / "settings.toml");
  config.Load();

  const Result<fs::path> installed = runner::InstallToolBinary(config, "gog", asset, "gogdl");
  REQUIRE(installed);
  CHECK(installed->filename() == "gogdl");
  CHECK(fs::exists(*installed));

  const auto perms = fs::status(*installed).permissions();
  CHECK((perms & fs::perms::owner_exec) != fs::perms::none);
}

TEST_CASE("InstallToolBinary finds a binary nested inside a zip, alongside sibling files") {
  // butler-linux-amd64.zip's shape:
  // everything nested under "linux-amd64/", the binary alongside shared
  // libraries it needs at runtime -- the whole reason InstallToolBinary
  // searches for binary_name instead of assuming a flat archive.
  const fs::path dir = TempDir("downloader-zip");
  const fs::path staging = dir / "staging";
  fs::create_directories(staging / "linux-amd64");
  std::ofstream(staging / "linux-amd64" / "butler") << "#!/bin/sh\necho hi\n";
  std::ofstream(staging / "linux-amd64" / "7z.so") << "not a real .so, just a fixture\n";

  const fs::path zip_path = dir / "butler-linux-amd64.zip";
  WriteZip(zip_path, staging / "linux-amd64");

  runner::ReleaseAsset asset;
  asset.tag = "v15.31.0";
  asset.asset_name = "butler-linux-amd64.zip";
  asset.download_url = "file://" + zip_path.string();

  config::Config config(dir / "settings.toml");
  config.Load();

  const Result<fs::path> installed = runner::InstallToolBinary(config, "itch", asset, "butler");
  REQUIRE(installed);
  CHECK(installed->filename() == "butler");
  CHECK(fs::exists(*installed));
  CHECK(fs::exists(installed->parent_path() / "7z.so"));  // sibling the binary needs at runtime

  const auto perms = fs::status(*installed).permissions();
  CHECK((perms & fs::perms::owner_exec) != fs::perms::none);
}

TEST_CASE("InstallToolBinary reports a clear error when the archive doesn't contain binary_name") {
  const fs::path dir = TempDir("downloader-zip-missing-binary");
  const fs::path staging = dir / "staging";
  fs::create_directories(staging / "linux-amd64");
  std::ofstream(staging / "linux-amd64" / "not-the-right-name") << "decoy\n";

  const fs::path zip_path = dir / "butler-linux-amd64.zip";
  WriteZip(zip_path, staging / "linux-amd64");

  runner::ReleaseAsset asset;
  asset.tag = "v15.31.0";
  asset.asset_name = "butler-linux-amd64.zip";
  asset.download_url = "file://" + zip_path.string();

  config::Config config(dir / "settings.toml");
  config.Load();

  const Result<fs::path> installed = runner::InstallToolBinary(config, "itch", asset, "butler");
  REQUIRE_FALSE(installed);
  CHECK(installed.error().code == "binary_not_found");
}

TEST_CASE("Installed builds map to their download source, and release names read cleanly") {
  test::TestEnv env("runner-families");
  const config::Config& config = env.config;

  const auto family = [&](const char* kind, const char* name, const char* folder) {
    const auto found = runner::FamilyOfBuild(config, kind, name, folder);
    return found ? found->id : std::string();
  };
  CHECK(family("wine", "wine-11.18-staging-tkg-amd64", "wine-11.18-staging-tkg-amd64") == "wine_staging_tkg");
  CHECK(family("wine", "wine-11.18-staging-amd64", "wine-11.18-staging-amd64") == "wine_staging");
  CHECK(family("wine", "wine-11.18-amd64", "wine-11.18-amd64") == "wine_vanilla");
  CHECK(family("wine", "lutris-GE-Proton8-26-x86_64", "lutris-GE-Proton8-26-x86_64") == "wine_ge");
  CHECK(family("proton", "cachyos-11.0-20260703-slr", "Proton-CachyOS Latest") == "proton_cachyos");
  CHECK(family("wine", "system", "usr").empty());

  const runner::ReleaseAsset wine_ge{.tag = "GE-Proton8-26", .asset_name = "wine-lutris-GE-Proton8-26-x86_64.tar.xz"};
  CHECK(runner::IsInstalledAs("wine", "lutris-GE-Proton8-26-x86_64", "lutris-GE-Proton8-26-x86_64", wine_ge));
  CHECK(runner::BuildLabel("wine", runner::ReleaseName("wine", wine_ge)) == "Wine-GE 8-26");
  CHECK(runner::BuildLabel("wine", "wine-11.18-staging-tkg-amd64") == "Wine 11.18 staging-tkg");
}
