#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "runner/Downloader.h"
#include "runner/Winetricks.h"
#include "support/TestEnv.h"
#include "updates/Updates.h"

using namespace mira;
using system::Family;
using test::TempDir;
namespace fs = std::filesystem;

TEST_CASE("ClassifyInstall tells how this copy of Mira updates") {
  const auto owned = [](const std::vector<std::string>&) { return true; };
  const auto unowned = [](const std::vector<std::string>&) { return false; };
  const fs::path packaged = "/usr/bin/mirad";

  CHECK(updates::ClassifyInstall("/home/a/Mira.AppImage", "/tmp/.mount/usr/bin/mirad", Family::Arch,
                                 unowned) == "appimage");
  CHECK(updates::ClassifyInstall("", "/home/a/Mira/build/mirad", Family::Debian, owned) == "none");
  CHECK(updates::ClassifyInstall("", packaged, Family::Debian, owned) == "deb");
  CHECK(updates::ClassifyInstall("", packaged, Family::Fedora, owned) == "rpm");
  CHECK(updates::ClassifyInstall("", packaged, Family::Suse, owned) == "rpm");
  // Installed some other way, or a package Mira can't replace itself.
  CHECK(updates::ClassifyInstall("", packaged, Family::Debian, unowned) == "manual");
  CHECK(updates::ClassifyInstall("", packaged, Family::Arch, owned) == "manual");
  CHECK(updates::ClassifyInstall("", packaged, Family::Ostree, owned) == "manual");
}

TEST_CASE("DownloadWithDigest keeps only a file matching its digest") {
  const fs::path dir = TempDir("self-update");
  const fs::path source = dir / "source";
  std::ofstream(source) << "hello\n";  // sha256 below
  const std::string hello = "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03";

  runner::ReleaseAsset asset;
  asset.asset_name = "Mira-x86_64.AppImage";
  asset.download_url = "file://" + source.string();
  const fs::path target = dir / "target";

  SUBCASE("a match stays") {
    asset.digest = "sha256:" + hello;
    REQUIRE(runner::DownloadWithDigest(asset, target));
    CHECK(fs::exists(target));
  }
  SUBCASE("a mismatch is removed") {
    asset.digest = "sha256:" + std::string(64, '0');
    const auto result = runner::DownloadWithDigest(asset, target);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "checksum_mismatch");
    CHECK_FALSE(fs::exists(target));
  }
  SUBCASE("no digest is refused before downloading") {
    const auto result = runner::DownloadWithDigest(asset, target);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "checksum_missing");
    CHECK_FALSE(fs::exists(target));
  }
  CHECK_FALSE(fs::exists(dir / "target.sha256sum"));
}

TEST_CASE("InstalledWinetricksVersion reads the version the script names") {
  const fs::path dir = TempDir("winetricks-version");
  const fs::path script = dir / "winetricks";
  std::ofstream(script) << "#!/bin/sh\nWINETRICKS_VERSION=20260125\necho hi\n";
  CHECK(runner::InstalledWinetricksVersion(script) == "20260125");
  CHECK(runner::InstalledWinetricksVersion(dir / "missing").empty());
}
