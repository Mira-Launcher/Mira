#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "api/EventBus.h"
#include "config/Config.h"
#include "epic/EpicImporter.h"
#include "epic/Legendary.h"
#include "library/Catalog.h"
#include "library/StoreProgress.h"
#include "store/GameStore.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

// A stand-in for the real `legendary`, pointed at by epic.legendary_bin.
// Deliberately a real executable rather than a mocked subprocess layer: the
// bugs this file exists to pin down were all in how legendary's *actual*
// output gets read, so the test has to go through a real fork/exec/pipe the
// same way RunAndWait does.
//
// `status_body`/`installed_body`/`list_body` are what it prints on stdout
// for each subcommand; `stderr_noise` is written to stderr first, which is
// the part that matters -- see the noise tests below.
// `install` prints a download progress line and from then on lists
// `after_install_body` as installed, the way a real install changes it.
void WriteFakeLegendary(const fs::path& path, const std::string& stderr_noise,
                        const std::string& status_body, const std::string& installed_body,
                        const std::string& list_body,
                        const std::string& after_install_body = "[]") {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      // %b, not %s: stderr_noise carries literal "\n" escapes that need
      // interpreting into real newlines for the line-based JSON scan
      // (ParseJsonTail) to see genuinely separate lines, the same as
      // legendary's own multi-line log output does.
      << "printf '%b' \"" << stderr_noise << "\" >&2\n"
      << "case \"$1\" in\n"
      << "  status) printf '%s\\n' '" << status_body << "' ;;\n"
      << "  list-installed) if [ -f \"$0.installed\" ]; then cat \"$0.installed\";"
      << " else printf '%s\\n' '" << installed_body << "'; fi ;;\n"
      << "  install)\n"
      << "    printf '%s\\n' '[DLManager] INFO: = Progress: 50.00% (1/2), ETA: 00:00:30' >&2\n"
      << "    printf '%s\\n' '" << after_install_body << "' > \"$0.installed\" ;;\n"
      << "  list) printf '%s\\n' '" << list_body << "' ;;\n"
      << "  --version) printf 'legendary version \"0.20.34\"\\n' ;;\n"
      << "esac\n"
      << "exit 0\n";
  out.close();
  fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
}

constexpr const char* kLoggedIn = R"({"account": "Tester", "games_available": 2, "games_installed": 1})";
constexpr const char* kLoggedOut = R"({"account": "<not logged in>", "games_available": 0, "games_installed": 0})";

// Real legendary log lines, tags and all -- "[Core]" starts with a literal
// '[' that a naive "find the first bracket" JSON scan mistakes for the
// opening of a top-level array.
constexpr const char* kNoise = "[Core] INFO: Trying to re-use existing login session...\\n[cli] INFO: Getting game list...\\n";

struct Fixture : test::TestEnv {
  explicit Fixture(const char* name) : TestEnv(name) {}

  void UseFakeLegendary(const std::string& noise, const std::string& status_body,
                        const std::string& installed_body = "[]", const std::string& list_body = "[]") {
    const fs::path bin = dir / "legendary";
    WriteFakeLegendary(bin, noise, status_body, installed_body, list_body);
    REQUIRE(config.Set("epic.legendary_bin", bin.string()));
  }
};

}  // namespace

TEST_CASE("DetectLegendary honours the epic.legendary_bin override") {
  Fixture fixture("epic-detect");
  fixture.UseFakeLegendary("", kLoggedOut);

  const runner::ToolStatus status = epic::DetectLegendary(fixture.config);
  CHECK(status.installed);
  CHECK(status.source == "override");
  CHECK(status.path == (fixture.dir / "legendary").string());
}

TEST_CASE("Status treats legendary's \"<not logged in>\" placeholder as unauthenticated") {
  Fixture fixture("epic-logged-out");
  fixture.UseFakeLegendary("", kLoggedOut);

  const runner::AuthStatus status = epic::Status(fixture.config);
  REQUIRE(status.tool.installed);
  CHECK_FALSE(status.authenticated);
  CHECK(status.account.empty());
}

TEST_CASE("Status reads the account through legendary's stderr log noise") {
  // The regression that actually shipped: legendary logs to stderr,
  // RunAndWait merges stderr into stdout, and the merged text is no longer
  // parseable as JSON from byte zero. Worse, the log lines are tagged
  // "[Core]"/"[cli]", so scanning for the first '{' or '[' finds the log
  // tag rather than the payload. Authenticated is exactly when legendary
  // has something to log about, so this broke only once logged in.
  Fixture fixture("epic-noisy-status");
  fixture.UseFakeLegendary(kNoise, kLoggedIn);

  const runner::AuthStatus status = epic::Status(fixture.config);
  REQUIRE(status.tool.installed);
  CHECK(status.authenticated);
  CHECK(status.account == "Tester");
}

TEST_CASE("Login rejects a pasted JSON page with no authorizationCode") {
  Fixture fixture("epic-login-json");
  fixture.UseFakeLegendary("", kLoggedOut);

  const Result<void> result = epic::Login(fixture.config, R"({"redirectUrl": "https://example"})");
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "invalid_code");
}

TEST_CASE("RunLegendaryJson reads a top-level array through the same noise") {
  // list/list-installed return an array, not an object -- the other half of
  // the same bug.
  Fixture fixture("epic-noisy-array");
  fixture.UseFakeLegendary(kNoise, kLoggedIn, R"([{"app_name": "abc", "title": "A Game"}])");

  const Result<nlohmann::json> listed = epic::RunLegendaryJson(fixture.config, {"list-installed"});
  REQUIRE(listed);
  REQUIRE(listed->is_array());
  REQUIRE(listed->size() == 1);
  CHECK((*listed)[0].value("app_name", std::string()) == "abc");
}

TEST_CASE("EpicImporter imports installed titles and tags them") {
  Fixture fixture("epic-import");
  fixture.UseFakeLegendary(
      kNoise, kLoggedIn,
      R"([{"app_name": "abc", "title": "A Game", "install_path": "/tmp/mira-tests/agame", "executable": "A.exe"}])");

  epic::EpicImporter importer(fixture.config, fixture.games, fixture.events);
  const Result<library::ImportSummary> summary = importer.Import();
  REQUIRE(summary);
  CHECK(summary->added == 1);
  CHECK(summary->updated == 0);

  const auto game = fixture.games.Find("epic-abc");
  REQUIRE(game);
  CHECK(game->name == "A Game");
  CHECK(game->source == "epic");
  CHECK(game->source_ref == "abc");  // the handle install/update/metadata all key off
  CHECK(game->exe_path == "A.exe");
  CHECK(game->platform == model::Platform::Windows);
  CHECK(std::ranges::find(game->tags, "epic") != game->tags.end());
  // Legendary makes no prefix of its own, so the importer has to name one
  // before provisioning -- the empty data_dir here is what broke a real
  // install with "game has no data_dir set".
  CHECK_FALSE(game->data_dir.empty());
}

TEST_CASE("EpicImporter is idempotent") {
  Fixture fixture("epic-import-twice");
  fixture.UseFakeLegendary(
      kNoise, kLoggedIn,
      R"([{"app_name": "abc", "title": "A Game", "install_path": "/tmp/mira-tests/agame", "executable": "A.exe"}])");

  epic::EpicImporter importer(fixture.config, fixture.games, fixture.events);
  REQUIRE(importer.Import());
  const Result<library::ImportSummary> second = importer.Import();
  REQUIRE(second);
  CHECK(second->added == 0);
  CHECK(second->updated == 1);
  CHECK(fixture.games.All().size() == 1);
}

TEST_CASE("ListCatalog reports entitlements read-through and marks tracked ones") {
  // `list` has two titles, only one of them installed: both are listed, but
  // only the installed one may end up in games.toml.
  Fixture fixture("epic-catalog");
  fixture.UseFakeLegendary(
      kNoise, kLoggedIn,
      R"([{"app_name": "abc", "title": "A Game", "install_path": "/tmp/mira-tests/agame", "executable": "A.exe"}])",
      R"([{"app_name": "abc", "app_title": "A Game"}, {"app_name": "xyz", "app_title": "Not Installed"}])");

  epic::EpicImporter importer(fixture.config, fixture.games, fixture.events);
  REQUIRE(importer.Import());
  CHECK(fixture.games.All().size() == 1);
  CHECK_FALSE(fixture.games.Find("epic-xyz"));

  const Result<std::vector<library::CatalogEntry>> entries =
      library::ListCatalog(fixture.config, fixture.games, "epic");
  REQUIRE(entries);
  REQUIRE(entries->size() == 2);

  const auto installed = std::ranges::find(*entries, "abc", &library::CatalogEntry::ref);
  const auto owned_only = std::ranges::find(*entries, "xyz", &library::CatalogEntry::ref);
  REQUIRE(installed != entries->end());
  REQUIRE(owned_only != entries->end());

  CHECK(installed->installed);
  CHECK(installed->game_id == "epic-abc");
  CHECK(owned_only->title == "Not Installed");
  CHECK_FALSE(owned_only->installed);
  CHECK(owned_only->game_id.empty());
}

TEST_CASE("Installing an owned Epic title reports progress, then tracks it as installed") {
  const fs::path state = test::TempDir("epic-api-install");
  const fs::path game_dir = state / "Games" / "AGame";
  test::Touch(game_dir / "A.exe");
  test::LiveServer server(state);
  WriteFakeLegendary(state / "legendary", kNoise, kLoggedIn, "[]",
                     R"([{"app_name": "abc", "app_title": "A Game"}])",
                     R"([{"app_name": "abc", "title": "A Game", "install_path": ")" +
                         game_dir.string() + R"(", "executable": "A.exe"}])");
  REQUIRE(server.MutableConfig().Set("epic.legendary_bin", (state / "legendary").string()));
  httplib::Client client = server.Client();
  const auto library = [&] {
    auto res = client.Get("/v1/library?source=epic");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    return nlohmann::json::parse(res->body);
  };

  REQUIRE(library().size() == 1);
  CHECK_FALSE(library()[0]["installed"].get<bool>());

  const auto install = [&](const std::string& ref) {
    const nlohmann::json body = {{"source", "epic"}, {"ref", ref}};
    return client.Post("/v1/library/install", body.dump(), "application/json");
  };
  auto started = install("abc");
  REQUIRE(started != nullptr);
  CHECK(started->status == 202);
  REQUIRE(test::WaitForEvent(server.events(), "library.install.finished"));
  const auto progress = test::WaitForEvent(server.events(), "library.install.progress");
  REQUIRE(progress);
  CHECK(progress->payload["progress"] == doctest::Approx(0.5));
  CHECK(progress->payload["eta"] == 30);

  const auto game = server.games().Find("epic-abc");
  REQUIRE(game);
  CHECK(game->install_path == game_dir.string());
  CHECK(library()[0]["installed"].get<bool>());
  CHECK(library()[0]["game_id"] == "epic-abc");

  auto bad = install("../x");
  REQUIRE(bad != nullptr);
  CHECK(bad->status == 400);
}

TEST_CASE("An Epic install while signed out fails with a way to sign in") {
  const fs::path state = test::TempDir("epic-api-install-signed-out");
  test::LiveServer server(state);
  WriteFakeLegendary(state / "legendary", "", kLoggedOut, "[]", "[]");
  REQUIRE(server.MutableConfig().Set("epic.legendary_bin", (state / "legendary").string()));
  httplib::Client client = server.Client();

  auto started = client.Post("/v1/library/install", R"({"source": "epic", "ref": "abc"})",
                             "application/json");
  REQUIRE(started != nullptr);
  const auto failed = test::WaitForEvent(server.events(), "library.install.failed");
  REQUIRE(failed);
  CHECK(failed->payload["ref"] == "abc");
  CHECK(failed->payload["code"] == "not_authenticated");
  CHECK(failed->payload["fix"]["kind"] == "source");
  CHECK(failed->payload["fix"]["target"] == "epic");
  CHECK(std::ranges::none_of(server.events().Since(0), [](const model::Event& event) {
    return event.type == "library.install.finished";
  }));
}

TEST_CASE("ParseProgressLine reads legendary and gogdl download lines") {
  library::DownloadProgress progress;
  CHECK(library::ParseProgressLine(
      "[DLManager] INFO: = Progress: 42.50% (425/1000), Running for 00:01:00, ETA: 00:02:05", progress));
  CHECK(progress.fraction == doctest::Approx(0.425));
  CHECK(progress.eta_seconds == 125);
  CHECK_FALSE(library::ParseProgressLine("[DLManager] INFO:  + Download\t- 12.50 MiB/s (raw)", progress));
  CHECK(progress.bytes_per_second == doctest::Approx(12.5 * 1024 * 1024));
  CHECK(library::ParseProgressLine("= Progress: 7.00 70/1000, Running for: 00:00:10, ETA: 00:02:10", progress));
  CHECK(progress.fraction == doctest::Approx(0.07));
}
