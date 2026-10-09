#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "api/EventBus.h"
#include "config/Config.h"
#include "epic/EpicImporter.h"
#include "epic/Legendary.h"
#include "library/Catalog.h"
#include "library/SourceRegistry.h"
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
      << "    printf '%s\\n' \"$*\" > \"$0.args\"\n"
      << "    printf '%s\\n' '[DLManager] INFO: = Progress: 50.00% (1/2), ETA: 00:00:30' >&2\n"
      << "    printf '%s\\n' '" << after_install_body << "' > \"$0.installed\" ;;\n"
      << "  list) printf '%s\\n' '" << list_body << "' ;;\n"
      << "  --version) printf 'legendary version \"0.20.34\"\\n' ;;\n"
      << "esac\n"
      << "exit 0\n";
  out.close();
  fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);

  // The saved session epic::Status reads, matching what `status` reports.
  const fs::path user = epic::LegendaryConfigDir() / "user.json";
  const std::string account = nlohmann::json::parse(status_body).value("account", "");
  if (account == "<not logged in>") {
    fs::remove(user);
  } else {
    fs::create_directories(user.parent_path());
    std::ofstream(user) << nlohmann::json{{"displayName", account}, {"refresh_expires_at", "2999-01-01T00:00:00.000Z"}};
  }
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

TEST_CASE("Status reads legendary's saved session: signed out, signed in, or expired") {
  Fixture fixture("epic-status");
  fixture.UseFakeLegendary("", kLoggedOut);
  runner::AuthStatus status = epic::Status(fixture.config);
  REQUIRE(status.tool.installed);
  CHECK_FALSE(status.authenticated);
  CHECK(status.account.empty());

  fixture.UseFakeLegendary(kNoise, kLoggedIn);
  status = epic::Status(fixture.config);
  CHECK(status.authenticated);
  CHECK(status.account == "Tester");

  // Past its refresh token's expiry legendary can't sign in without a new code.
  std::ofstream(epic::LegendaryConfigDir() / "user.json")
      << R"({"displayName": "Tester", "refresh_expires_at": "2020-01-01T00:00:00.000Z"})";
  CHECK_FALSE(epic::Status(fixture.config).authenticated);
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

TEST_CASE("The Epic catalog reports every entitlement and marks tracked ones") {
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
      library::FindSource("epic")->Catalog(fixture.config, fixture.games);
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

TEST_CASE("The library listing answers from the stored list and re-checks the store behind it") {
  const fs::path state = test::TempDir("epic-api-stored");
  test::LiveServer server(state);
  const auto fake = [&](const std::string& list) {
    WriteFakeLegendary(state / "legendary", kNoise, kLoggedIn, "[]", list);
  };
  fake(R"([{"app_name": "abc", "app_title": "A Game"}])");
  REQUIRE(server.MutableConfig().Set("epic.legendary_bin", (state / "legendary").string()));
  httplib::Client client = server.Client();
  const auto titles = [&](const std::string& query = "") {
    auto res = client.Get("/v1/library?source=epic" + query);
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    std::vector<std::string> out;
    for (const auto& entry : nlohmann::json::parse(res->body)) out.push_back(entry["title"]);
    return out;
  };
  const auto last_event = [&] {
    const auto events = server.events().Since(0);
    return events.empty() ? std::int64_t{0} : events.back().id;
  };
  // The first re-check of epic finished after event `after`, and whether it changed the list.
  const auto checked = [&](std::int64_t after) -> std::optional<bool> {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      for (const auto& event : server.events().Since(after)) {
        if (event.type == "library.catalog_checked" && event.payload["source"] == "epic") {
          return event.payload["changed"].get<bool>();
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
  };

  // Never listed: asked at once.
  CHECK(titles() == std::vector<std::string>{"A Game"});

  fake(R"([{"app_name": "abc", "app_title": "A Game"}, {"app_name": "xyz", "app_title": "Another"}])");
  std::int64_t before = last_event();
  CHECK(titles() == std::vector<std::string>{"A Game"});
  CHECK(checked(before) == true);
  before = last_event();
  CHECK(titles() == std::vector<std::string>{"A Game", "Another"});
  CHECK(checked(before) == false);

  // Steam's reviews of a title come with it, as a share of positive ones.
  REQUIRE(server.games().Metadata().Write(
      "epic-xyz", {{"steam_reviews",
                    {{"score_description", "Very Positive"}, {"total_positive", 87}, {"total_negative", 13},
                     {"total_reviews", 100}}}}));
  before = last_event();
  auto listed = client.Get("/v1/library?source=epic");
  REQUIRE(listed != nullptr);
  const auto entries = nlohmann::json::parse(listed->body);
  REQUIRE(entries.size() == 2);
  CHECK_FALSE(entries[0].contains("steam_reviews"));
  CHECK(entries[1]["steam_reviews"] ==
        nlohmann::json{{"score_description", "Very Positive"}, {"percent_positive", 87}, {"total_reviews", 100}});
  REQUIRE(checked(before).has_value());

  // `fresh` asks now.
  fake("[]");
  CHECK(titles("&fresh=1").empty());
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
  // Into Mira's Epic folder, not legendary's own default.
  std::ifstream args_file(state / "legendary.args");
  std::string args;
  std::getline(args_file, args);
  CHECK(args.find("--base-path " + server.config().GetPath("epic.install_root").string()) != std::string::npos);
  CHECK(library()[0]["game_id"] == "epic-abc");

  auto bad = install("../x");
  REQUIRE(bad != nullptr);
  CHECK(bad->status == 400);
}

TEST_CASE("Pausing an Epic install stops legendary gracefully and installing again resumes it") {
  const fs::path state = test::TempDir("epic-api-pause");
  const fs::path game_dir = state / "Games" / "AGame";
  test::Touch(game_dir / "A.exe");
  test::LiveServer server(state);
  WriteFakeLegendary(state / "legendary", kNoise, kLoggedIn, "[]", R"([{"app_name": "abc", "app_title": "A Game"}])",
                     R"([{"app_name": "abc", "title": "A Game", "install_path": ")" + game_dir.string() +
                         R"(", "executable": "A.exe"}])");
  // The first install stalls until stopped and notes the SIGTERM; the second one finishes.
  const fs::path slow = state / "legendary-slow";
  std::ofstream(slow) << "#!/bin/sh\n"
                      << "if [ \"$1\" = install ] && [ ! -f \"$0.started\" ]; then\n"
                      << "  touch \"$0.started\"\n"
                      << "  trap 'touch \"$0.term\"; exit 1' TERM\n"
                      << "  printf '%s\\n' '[DLManager] INFO: = Progress: 10.00% (1/10), ETA: 00:01:30' >&2\n"
                      << "  sleep 30 & wait\n"
                      << "fi\n"
                      << "exec \"" << (state / "legendary").string() << "\" \"$@\"\n";
  fs::permissions(slow, fs::perms::owner_all);
  REQUIRE(server.MutableConfig().Set("epic.legendary_bin", slow.string()));
  httplib::Client client = server.Client();
  const std::string body = nlohmann::json{{"source", "epic"}, {"ref", "abc"}}.dump();

  REQUIRE(client.Post("/v1/library/install", body, "application/json")->status == 202);
  REQUIRE(test::WaitForEvent(server.events(), "library.install.progress"));
  auto paused = client.Post("/v1/library/install/pause", body, "application/json");
  REQUIRE(paused != nullptr);
  CHECK(paused->status == 200);
  REQUIRE(test::WaitForEvent(server.events(), "library.install.paused"));
  CHECK(fs::exists(state / "legendary-slow.term"));
  CHECK(nlohmann::json::parse(client.Get("/v1/library/install/paused")->body).size() == 1);
  CHECK(client.Post("/v1/library/install/pause", body, "application/json")->status == 409);

  REQUIRE(client.Post("/v1/library/install", body, "application/json")->status == 202);
  REQUIRE(test::WaitForEvent(server.events(), "library.install.finished"));
  CHECK(server.games().Find("epic-abc"));
  CHECK(nlohmann::json::parse(client.Get("/v1/library/install/paused")->body).empty());
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
