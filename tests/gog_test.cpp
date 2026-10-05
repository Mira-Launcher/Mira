#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Paths.h"
#include "gog/Gog.h"
#include "gog/GogImporter.h"
#include "store/GameStore.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

// A stand-in for the real `gogdl`, pointed at by gog.gogdl_bin --
// mirrors epic_test.cpp's WriteFakeLegendary (real executable, real
// fork/exec/pipe, not a mocked subprocess layer). Unlike Legendary, gogdl
// itself owns writing its --auth-config-path token file -- the fake here
// does the same, so gog::AccessToken's read-back path is exercised for
// real, not assumed.
void WriteFakeGogdl(const fs::path& path, const std::string& auth_success_body, const std::string& import_body) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      // Global "--auth-config-path <path>" always comes first (RunGogdl's
      // own convention) -- except VersionOf's own "--version" probe,
      // which calls the binary directly with no wrapper args at all, so
      // this only shifts when that flag is actually present.
      << "if [ \"$1\" = \"--auth-config-path\" ]; then\n"
      << "  auth_path=\"$2\"\n"
      << "  shift 2\n"
      << "fi\n"
      << "case \"$1\" in\n"
      << "  auth)\n"
      << "    if [ \"$2\" = \"--code\" ]; then\n"
      << "      if [ \"$3\" = \"bad-code\" ]; then printf '{\"error\": true}\\n'; exit 0; fi\n"
      << "    fi\n"
      // Both a fresh `--code` login and a bare refresh call end up here --
      // real gogdl rewrites the token file (with a fresh loginTime) either
      // way, which is what makes AccessToken's refresh-by-invoking-auth
      // trick work.
      << "    printf '%s' '" << auth_success_body << "' > \"$auth_path\"\n"
      << "    cat \"$auth_path\"\n"
      << "    ;;\n"
      << "  import) printf '%s\\n' '" << import_body << "' ;;\n"
      << "  download) ;;\n"
      << "  --version) printf 'gogdl version 1.3.0\\n' ;;\n"
      << "esac\n"
      << "exit 0\n";
  out.close();
  fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
}

// gogdl nests the fields one level down, keyed by client_id (see Gog.cpp's ReadAuthConfig).
constexpr const char* kFreshToken =
    R"({"46899977096215655": {"access_token": "tok", "refresh_token": "ref", )"
    R"("expires_in": 3600, "loginTime": 9999999999}})";

struct Fixture : test::TestEnv {
  explicit Fixture(const char* name) : TestEnv(name) {}

  void UseFakeGogdl(const std::string& auth_success_body = kFreshToken,
                    const std::string& import_body = R"({"title": "A GOG Game"})") {
    const fs::path bin = dir / "gogdl";
    WriteFakeGogdl(bin, auth_success_body, import_body);
    REQUIRE(config.Set("gog.gogdl_bin", bin.string()));
  }
};

}  // namespace

TEST_CASE("DetectGog honours the gog.gogdl_bin override") {
  Fixture fixture("gog-detect");
  fixture.UseFakeGogdl();

  const runner::ToolStatus status = gog::DetectGog(fixture.config);
  CHECK(status.installed);
  CHECK(status.source == "override");
  CHECK(status.path == (fixture.dir / "gogdl").string());
}

TEST_CASE("Login stores what gogdl wrote, and Status reads it back") {
  Fixture fixture("gog-login");
  fixture.UseFakeGogdl();

  REQUIRE(gog::Login(fixture.config, "good-code"));
  const gog::GogAuthStatus status = gog::Status(fixture.config);
  REQUIRE(status.gogdl.installed);
  CHECK(status.authenticated);
}

TEST_CASE("Login fails when gogdl reports {\"error\": true} despite exiting 0") {
  // `gogdl auth --code <bogus>` prints {"error": true} but
  // still exits 0 -- same exit-code lie Legendary has, and the same fix
  // (verify via a real status read-back, not the exit code).
  Fixture fixture("gog-login-bad-code");
  fixture.UseFakeGogdl();

  const Result<void> result = gog::Login(fixture.config, "bad-code");
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "login_failed");
  CHECK_FALSE(gog::Status(fixture.config).authenticated);
}

TEST_CASE("Login pulls the code out of a pasted redirect URL") {
  Fixture fixture("gog-login-url");
  fixture.UseFakeGogdl();

  // The fake rejects exactly "bad-code", so a failure proves the bare code
  // (and not the whole URL) reached gogdl.
  const Result<void> result =
      gog::Login(fixture.config, "  https://embed.gog.com/on_login_success?origin=client&code=bad-code&x=1\n");
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "login_failed");
  CHECK(gog::Login(fixture.config, "https://embed.gog.com/on_login_success?origin=client&code=good-code"));
}

TEST_CASE("AccessToken refreshes an expired token by re-invoking gogdl auth") {
  Fixture fixture("gog-token-refresh");
  fixture.UseFakeGogdl();

  // Write an already-expired token file directly, bypassing gogdl -- this
  // is the state Mira finds on a real second run after the token's
  // expires_in has elapsed.
  std::ofstream expired(gog::AuthConfigPath(fixture.config));
  expired << R"({"46899977096215655": {"access_token": "stale", "refresh_token": "ref", )"
             R"("expires_in": 60, "loginTime": 0}})";
  expired.close();

  const Result<std::string> token = gog::AccessToken(fixture.config);
  REQUIRE(token);
  CHECK(*token == "tok");  // the fake's refresh path writes this, not "stale"
}

TEST_CASE("GogImporter::ImportPath tags and provisions a title") {
  Fixture fixture("gog-import");
  fixture.UseFakeGogdl();

  gog::GogImporter importer(fixture.config, fixture.games, fixture.events);
  const Result<model::Game> imported = importer.ImportPath("123", fixture.dir / "installed" / "123");
  REQUIRE(imported);
  CHECK(imported->name == "A GOG Game");
  CHECK(imported->source == "gog");
  CHECK(imported->source_ref == "123");
  CHECK(std::ranges::find(imported->tags, "gog") != imported->tags.end());
  CHECK_FALSE(imported->data_dir.empty());

  const auto game = fixture.games.Find("gog-123");
  REQUIRE(game);
  CHECK(game->name == "A GOG Game");
}

TEST_CASE("GogImporter::Import scans install_root's subdirectories") {
  Fixture fixture("gog-import-scan");
  fixture.UseFakeGogdl();

  const fs::path root = fixture.dir / "install_root";
  fs::create_directories(root / "123");
  fs::create_directories(root / "456");
  REQUIRE(fixture.config.Set("gog.install_root", root.string()));

  gog::GogImporter importer(fixture.config, fixture.games, fixture.events);
  const Result<gog::GogImportSummary> summary = importer.Import();
  REQUIRE(summary);
  CHECK(summary->added == 2);
  CHECK(fixture.games.All().size() == 2);
}

TEST_CASE("GogImporter::Import reads the id from goggame-<id>.info in a title-named folder") {
  Fixture fixture("gog-import-title-dir");
  fixture.UseFakeGogdl();

  const fs::path root = fixture.dir / "install_root";
  fs::create_directories(root / "Hollow Knight" / "Hollow Knight_Data");
  std::ofstream(root / "Hollow Knight" / "goggame-1328670078.info") << "{}";
  REQUIRE(fixture.config.Set("gog.install_root", root.string()));

  CHECK(gog::FindGameDir(fixture.config, "1328670078") == root / "Hollow Knight");

  gog::GogImporter importer(fixture.config, fixture.games, fixture.events);
  REQUIRE(importer.Import());
  const auto game = fixture.games.Find("gog-1328670078");
  REQUIRE(game);
  CHECK(game->install_path == (root / "Hollow Knight").string());
}

TEST_CASE("GOG sign-in through mirad refuses a bad code and keeps a good one until sign-out") {
  const fs::path state = test::TempDir("gog-api-sign-in");
  test::LiveServer server(state);
  WriteFakeGogdl(state / "gogdl", kFreshToken, "{}");
  REQUIRE(server.MutableConfig().Set("gog.gogdl_bin", (state / "gogdl").string()));
  httplib::Client client = server.Client();
  const auto status = [&] {
    auto res = client.Get("/v1/gog/status");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    return nlohmann::json::parse(res->body);
  };
  const auto sign_in = [&](const std::string& code) {
    auto res =
        client.Post("/v1/gog/auth", nlohmann::json{{"code", code}}.dump(), "application/json");
    REQUIRE(res != nullptr);
    return res->status;
  };

  const nlohmann::json before = status();
  CHECK(before["gogdl"]["installed"].get<bool>());
  CHECK_FALSE(before["authenticated"].get<bool>());
  CHECK_FALSE(before["login_url"].get<std::string>().empty());

  CHECK(sign_in("bad-code") == 400);
  CHECK_FALSE(status()["authenticated"].get<bool>());
  CHECK(sign_in("https://embed.gog.com/on_login_success?origin=client&code=good-code") == 200);
  CHECK(status()["authenticated"].get<bool>());

  auto out = client.Post("/v1/gog/logout");
  REQUIRE(out != nullptr);
  CHECK(out->status == 200);
  CHECK_FALSE(status()["authenticated"].get<bool>());
}
