#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "humble/Humble.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

// A stand-in for the real `humble-cli`, pointed at by humble.humble_cli_bin.
// humble-cli's --field output is plain CSV, no header row (confirmed
// live: "pS5kGAW5APbRTHH7,Surviving Mars - Deluxe Edition,Yes") -- not
// the padded table an earlier version of this fixture assumed.
void WriteFakeHumbleCli(const fs::path& path, bool authenticated, const std::string& list_output) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      << "case \"$1\" in\n"
      << "  auth) exit 0 ;;\n"
      << "  list)\n";
  if (authenticated) {
    out << "    printf '%b' \"" << list_output << "\"\n";
  } else {
    out << "    echo 'Error: config file not found. Use '\"'\"'humble-cli auth <SESSION-KEY>'\"'\"' to set it' >&2\n"
        << "    exit 1\n";
  }
  out << "    ;;\n"
      << "  --version) printf 'humble-cli version 0.23.2\\n' ;;\n"
      << "esac\n"
      << "exit 0\n";
  out.close();
  fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
}

constexpr const char* kCsv =
    "abc123def456,Indie Bundle 42,Yes\\n"
    "xyz789,RPG Maker Bundle,No\\n";

struct Fixture {
  fs::path dir;
  config::Config config;

  explicit Fixture(const char* name) : dir(TempDir(name)), config(dir / "settings.toml") { config.Load(); }

  void UseFakeHumbleCli(bool authenticated, const std::string& list_output = kCsv) {
    const fs::path bin = dir / "humble-cli";
    WriteFakeHumbleCli(bin, authenticated, list_output);
    REQUIRE(config.Set("humble.humble_cli_bin", bin.string()));
  }
};

}  // namespace

TEST_CASE("DetectHumbleCli honours the humble.humble_cli_bin override") {
  Fixture fixture("humble-detect");
  fixture.UseFakeHumbleCli(true);

  const humble::HumbleStatus status = humble::DetectHumbleCli(fixture.config);
  CHECK(status.installed);
  CHECK(status.source == "override");
}

TEST_CASE("Status reports unauthenticated when humble-cli has no session key yet") {
  Fixture fixture("humble-status-unauth");
  fixture.UseFakeHumbleCli(false);

  const humble::HumbleAuthStatus status = humble::Status(fixture.config);
  REQUIRE(status.humble_cli.installed);
  CHECK_FALSE(status.authenticated);
}

TEST_CASE("Status reports authenticated once a session key works") {
  Fixture fixture("humble-status-auth");
  fixture.UseFakeHumbleCli(true);

  const humble::HumbleAuthStatus status = humble::Status(fixture.config);
  REQUIRE(status.humble_cli.installed);
  CHECK(status.authenticated);
}

TEST_CASE("ListBundles parses real CSV output, claimed case-insensitively") {
  Fixture fixture("humble-list");
  fixture.UseFakeHumbleCli(true);

  const Result<std::vector<humble::BundleSummary>> bundles = humble::ListBundles(fixture.config);
  REQUIRE(bundles);
  REQUIRE(bundles->size() == 2);

  CHECK((*bundles)[0].key == "abc123def456");
  CHECK((*bundles)[0].name == "Indie Bundle 42");
  CHECK((*bundles)[0].claimed);

  CHECK((*bundles)[1].key == "xyz789");
  CHECK((*bundles)[1].name == "RPG Maker Bundle");
  CHECK_FALSE((*bundles)[1].claimed);
}

TEST_CASE("ListBundles handles a quoted name containing a comma") {
  Fixture fixture("humble-list-comma");
  fixture.UseFakeHumbleCli(true, "k1,\\\"Foo, Bar Edition\\\",Yes\\n");

  const Result<std::vector<humble::BundleSummary>> bundles = humble::ListBundles(fixture.config);
  REQUIRE(bundles);
  REQUIRE(bundles->size() == 1);
  CHECK((*bundles)[0].name == "Foo, Bar Edition");
}
