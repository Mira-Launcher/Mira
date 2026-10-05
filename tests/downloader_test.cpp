#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"
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

namespace {

// A release as GitHub serves it: `name`.tar.gz holding a `name` folder, beside its checksum file.
runner::ReleaseAsset Release(const fs::path& dir, const std::string& name,
                             const std::string& checksum_name, const std::string& checksum_text) {
  RunOrFail({"tar", "-czf", (dir / (name + ".tar.gz")).string(), name}, dir);
  test::Touch(dir / checksum_name, checksum_text);
  return {.tag = name,
          .asset_name = name + ".tar.gz",
          .download_url = "file://" + (dir / (name + ".tar.gz")).string(),
          .checksum_url = "file://" + (dir / checksum_name).string()};
}

std::string Checksum(const char* tool, const fs::path& file) {
  Command command;
  command.argv = {tool, file.filename().string()};
  command.cwd = file.parent_path();
  const auto result = runner::RunAndWait(command);
  REQUIRE(result);
  return result->output;  // "<hash>  <name>\n"
}

// Nothing a failed install could leave for Discover to pick up.
bool OnlyHolds(const fs::path& dir, const std::vector<std::string>& names) {
  std::vector<std::string> found;
  for (const auto& entry : fs::directory_iterator(dir)) {
    found.push_back(entry.path().filename().string());
  }
  std::ranges::sort(found);
  return found == names;
}

}  // namespace

TEST_CASE("A Proton download installs only when its checksum matches, and leaves nothing else") {
  test::TestEnv env("downloader-proton");
  const fs::path releases = env.dir / "releases";
  test::Touch(releases / "GE-Proton9-1" / "proton");
  test::Touch(releases / "GE-Proton9-1" / "toolmanifest.vdf");
  test::Touch(releases / "GE-Proton9-1" / "version", "1700000000 GE-Proton9-1");
  const fs::path runners = env.dir / "runners" / "proton";

  // A tampered download: the checksum is for other bytes.
  runner::ReleaseAsset bad = Release(releases, "GE-Proton9-1", "GE-Proton9-1.sha512sum",
                                     std::string(128, '0') + "  GE-Proton9-1.tar.gz\n");
  const auto refused = runner::DownloadAndInstall(env.config, "proton", bad);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().code == "checksum_mismatch");
  CHECK(OnlyHolds(runners, {}));

  test::Touch(releases / "GE-Proton9-1.sha512sum",
              Checksum("sha512sum", releases / "GE-Proton9-1.tar.gz"));
  REQUIRE(runner::DownloadAndInstall(env.config, "proton", bad));
  CHECK(OnlyHolds(runners, {"GE-Proton9-1"}));
  CHECK(runner::RunnerRegistry(env.config).Resolve("proton:GE-Proton9-1"));
}

TEST_CASE("A Wine download is checked against its line in a shared checksum list") {
  test::TestEnv env("downloader-wine");
  const fs::path releases = env.dir / "releases";
  test::Touch(releases / "wine-9.0-amd64" / "bin" / "wine", "#!/bin/sh\necho wine-9.0\n",
              /*executable=*/true);
  const fs::path runners = env.dir / "runners" / "wine";

  runner::ReleaseAsset asset = Release(releases, "wine-9.0-amd64", "sha256sums.txt", "");
  asset.checksum_is_sha256 = true;
  asset.checksum_is_list = true;
  const std::string others = std::string(64, 'a') + "  wine-9.0-staging-amd64.tar.gz\n";

  // A list that doesn't name this archive verifies nothing, so it's refused.
  test::Touch(releases / "sha256sums.txt", others);
  const auto unlisted = runner::DownloadAndInstall(env.config, "wine", asset);
  REQUIRE_FALSE(unlisted);
  CHECK(unlisted.error().code == "checksum_missing");
  CHECK(OnlyHolds(runners, {}));

  // Other entries in the list don't matter, only this archive's line.
  test::Touch(releases / "sha256sums.txt",
              others + Checksum("sha256sum", releases / "wine-9.0-amd64.tar.gz"));
  REQUIRE(runner::DownloadAndInstall(env.config, "wine", asset));
  CHECK(OnlyHolds(runners, {"wine-9.0-amd64"}));
  CHECK(runner::RunnerRegistry(env.config).Resolve("wine:wine-9.0-amd64"));
}
