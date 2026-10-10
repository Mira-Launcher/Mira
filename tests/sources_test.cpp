#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include <json.hpp>

#include "gog/Gog.h"
#include "migrate/Sources.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
using test::AwaitJob;
using test::LiveServer;
using test::TempDir;
using test::Touch;
namespace fs = std::filesystem;

namespace {

void WriteManifest(const fs::path& library, const std::string& appid, const std::string& name,
                   const std::string& installdir) {
  Touch(library / "steamapps" / ("appmanifest_" + appid + ".acf"),
        "\"AppState\"\n{\n\t\"appid\"\t\t\"" + appid + "\"\n\t\"name\"\t\t\"" + name +
            "\"\n\t\"installdir\"\t\t\"" + installdir + "\"\n}\n");
}

json Get(httplib::Client& client, const std::string& path) {
  auto res = client.Get(path);
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);
  return json::parse(res->body);
}

// What every source's games look like in the library.
model::Game SourceGame(const std::string& id, const std::string& source,
                       const fs::path& install_path, const std::string& data_dir = "") {
  model::Game game;
  game.id = id;
  game.name = id;
  game.source = source;
  game.install_path = install_path.string();
  game.data_dir = data_dir;
  game.status = model::GameStatus::Ready;
  return game;
}

}  // namespace

TEST_CASE("A Steam scan adds games from every library but Valve's tools, and keeps user edits") {
  const fs::path state = TempDir("sources-steam-scan");
  const fs::path steam = state / "steam";
  const fs::path second = state / "library2";
  Touch(steam / "steamapps" / "libraryfolders.vdf",
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + steam.string() +
            "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + second.string() + "\"\n\t}\n}\n");
  // Native: no compatdata.
  WriteManifest(steam, "570", "Dota 2", "dota 2 beta");
  fs::create_directories(steam / "steamapps" / "common" / "dota 2 beta");
  // Windows, in the second library: Steam made it a prefix.
  WriteManifest(second, "1145360", "Hades", "Hades");
  fs::create_directories(second / "steamapps" / "common" / "Hades");
  fs::create_directories(second / "steamapps" / "compatdata" / "1145360" / "pfx");
  // Never listed: a Proton build, the redistributables, and a manifest with nothing on disk.
  WriteManifest(second, "2805730", "Proton 9.0", "Proton 9.0");
  Touch(second / "steamapps" / "common" / "Proton 9.0" / "proton");
  WriteManifest(steam, "228980", "Steamworks Common Redistributables", "Steamworks Shared");
  fs::create_directories(steam / "steamapps" / "common" / "Steamworks Shared");
  WriteManifest(steam, "999", "Uninstalled", "Gone");

  LiveServer server(state);
  REQUIRE(server.MutableConfig().Set("steam.root", steam.string()).has_value());
  httplib::Client client = server.Client();

  const json first = AwaitJob(client, client.Post("/v1/steam/scan"));
  REQUIRE(first["state"] == "finished");
  CHECK(first["result"]["added"] == 2);
  CHECK(server.games().All().size() == 2);

  const json dota = Get(client, "/v1/games/steam-570");
  CHECK(dota["runner_ref"] == "steam:570");
  CHECK(dota["platform"] == "native");
  CHECK(dota["status"] == "ready");
  const json hades = Get(client, "/v1/games/steam-1145360");
  CHECK(hades["platform"] == "windows");
  CHECK(hades["install_path"] == (second / "steamapps" / "common" / "Hades").string());
  CHECK(hades["data_dir"] == (second / "steamapps" / "compatdata" / "1145360").string());

  REQUIRE(client.Patch("/v1/games/steam-570", R"({"tags": ["favorite"], "args": "-novid"})",
                       "application/json"));
  const json again = AwaitJob(client, client.Post("/v1/steam/scan"));
  CHECK(again["result"]["added"] == 0);
  CHECK(again["result"]["updated"] == 2);
  const json kept = Get(client, "/v1/games/steam-570");
  CHECK(kept["tags"] == json::array({"favorite"}));
  CHECK(kept["args"] == "-novid");
}

TEST_CASE("Removing Steam forgets its games but never touches their files") {
  const fs::path state = TempDir("sources-remove-steam");
  LiveServer server(state);
  const fs::path game_dir = state / "steamapps" / "common" / "Hades";
  fs::create_directories(game_dir);
  REQUIRE(server.games().Upsert(SourceGame("steam-1145360", "steam", game_dir)));
  REQUIRE(server.games().Upsert(SourceGame("celeste", "scan", state / "Celeste")));
  httplib::Client client = server.Client();

  const json plan = Get(client, "/v1/sources/steam/removal");
  REQUIRE(plan["games"].size() == 1);
  CHECK(plan["games"][0]["deletes"] == "");
  CHECK_FALSE(plan["signs_out"].get<bool>());

  const json job = AwaitJob(client, client.Post("/v1/sources/steam/remove"));
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["removed"] == 1);
  CHECK(job["result"]["problems"].empty());
  CHECK(fs::exists(game_dir));
  CHECK_FALSE(server.games().Find("steam-1145360"));
  CHECK(server.games().Find("celeste"));
  CHECK_FALSE(server.games().Source("steam").added);
}

TEST_CASE("Removing a store deletes only inside Mira's folders, keeps prefixes, and signs out") {
  const fs::path state = TempDir("sources-remove-gog");
  const fs::path install_root = state / "GOG Games";
  const fs::path outside = TempDir("sources-remove-gog-outside");
  LiveServer server(state);
  REQUIRE(server.MutableConfig().Set("gog.install_root", install_root.string()).has_value());
  Touch(install_root / "Celeste" / "celeste.exe");
  Touch(outside / "Hades" / "hades.exe");
  Touch(state / "prefixes" / "celeste" / "drive_c" / "users" / "save.dat");
  REQUIRE(server.games().Upsert(SourceGame("gog-1", "gog", install_root / "Celeste",
                                           (state / "prefixes" / "celeste").string())));
  REQUIRE(server.games().Upsert(SourceGame("gog-2", "gog", outside / "Hades")));
  Touch(gog::AuthConfigPath(server.config()), "{}");
  httplib::Client client = server.Client();

  const json plan = Get(client, "/v1/sources/gog/removal");
  CHECK(plan["signs_out"].get<bool>());
  CHECK(plan["kept"] == json::array({(state / "prefixes" / "celeste").string()}));

  const json job = AwaitJob(client, client.Post("/v1/sources/gog/remove"));
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["removed"] == 2);
  // The game outside every Mira folder is reported and left on disk.
  REQUIRE(job["result"]["problems"].size() == 1);
  CHECK(fs::exists(outside / "Hades" / "hades.exe"));
  CHECK_FALSE(fs::exists(install_root / "Celeste"));
  CHECK(fs::exists(state / "prefixes" / "celeste" / "drive_c" / "users" / "save.dat"));
  CHECK_FALSE(fs::exists(gog::AuthConfigPath(server.config())));
  CHECK(server.games().All().empty());
  CHECK_FALSE(server.games().Source("gog").added);
}

TEST_CASE("A launcher's games import from its prefix, and removing it keeps saves and the prefix") {
  const fs::path state = TempDir("sources-ubisoft");
  const fs::path prefix = state / "prefixes" / "ubisoft-connect";
  const fs::path program =
      prefix / "drive_c" / "Program Files (x86)" / "Ubisoft" / "Ubisoft Game Launcher";
  Touch(program / "UbisoftConnect.exe");
  Touch(program / "cache" / "assets.bin");
  Touch(program / "savegames" / "5595" / "1.save");
  Touch(program / "games" / "Trackmania" / "Trackmania.exe");
  Touch(prefix / "system.reg",
        "WINE REGISTRY Version 2\n\n"
        "[Software\\\\Wow6432Node\\\\Ubisoft\\\\Launcher\\\\Installs\\\\5595] 1700000000\n"
        "\"InstallDir\"=\"C:/Program Files (x86)/Ubisoft/Ubisoft Game "
        "Launcher/games/Trackmania/\"\n");

  LiveServer server(state);
  model::Game host = SourceGame("launcher-ubisoft", "launcher", program, prefix.string());
  host.source_ref = "ubisoft";
  host.exe_path = "UbisoftConnect.exe";
  REQUIRE(server.games().Upsert(host));
  httplib::Client client = server.Client();

  const json imported = AwaitJob(client, client.Post("/v1/launchers/ubisoft/import"));
  REQUIRE(imported["state"] == "finished");
  CHECK(imported["result"]["added"] == 1);
  const json trackmania = Get(client, "/v1/games/ubisoft-5595");
  CHECK(trackmania["install_path"] == (program / "games" / "Trackmania").string());
  CHECK(trackmania["data_dir"] == prefix.string());
  CHECK(trackmania["exe_path"] == "Trackmania.exe");
  CHECK(trackmania["source"] == "ubisoft");

  const json plan = Get(client, "/v1/sources/ubisoft/removal");
  REQUIRE(plan["games"].size() == 1);
  CHECK(plan["launcher_dir"] == program.string());
  CHECK(plan["kept"] == json::array({prefix.string(), (program / "savegames").string()}));

  const json removed = AwaitJob(client, client.Post("/v1/sources/ubisoft/remove"));
  REQUIRE(removed["state"] == "finished");
  CHECK(removed["result"]["removed"] == 2);
  CHECK(removed["result"]["problems"].empty());
  CHECK_FALSE(fs::exists(program / "games"));
  CHECK_FALSE(fs::exists(program / "UbisoftConnect.exe"));
  CHECK_FALSE(fs::exists(program / "cache"));
  CHECK(fs::exists(program / "savegames" / "5595" / "1.save"));
  CHECK(fs::exists(prefix / "system.reg"));
  CHECK(server.games().All().empty());
}

TEST_CASE("Sources can be switched off, reordered, and listed in that order") {
  LiveServer server(TempDir("sources-list"));
  httplib::Client client = server.Client();
  auto patched = client.Patch("/v1/sources/epic", R"({"enabled": false, "in_sidebar": false})", "application/json");
  REQUIRE(patched != nullptr);
  CHECK(patched->status == 200);
  auto ordered = client.Put("/v1/sources/order", R"({"order": ["gog", "local"]})", "application/json");
  REQUIRE(ordered != nullptr);
  CHECK(ordered->status == 200);

  const json sources = Get(client, "/v1/sources")["sources"];
  CHECK(sources[0]["id"] == "gog");
  CHECK(sources[1]["id"] == "local");
  CHECK(sources[1]["added"] == true);
  json epic;
  for (const json& source : sources) {
    if (source["id"] == "epic") epic = source;
  }
  CHECK(epic["enabled"] == false);
  CHECK(epic["in_sidebar"] == false);
}

TEST_CASE("A switched-off source's games are left out of the library unless include_off is set") {
  const fs::path state = TempDir("sources-off-games");
  LiveServer server(state);
  REQUIRE(server.games().Upsert(SourceGame("steam-570", "steam", state / "Dota 2")));
  httplib::Client client = server.Client();
  auto patched = client.Patch("/v1/sources/steam", R"({"enabled": false})", "application/json");
  REQUIRE(patched != nullptr);
  CHECK(patched->status == 200);

  const auto ids = [](const json& games) {
    std::vector<std::string> out;
    for (const json& game : games) out.push_back(game["id"]);
    return out;
  };
  CHECK_FALSE(std::ranges::contains(ids(Get(client, "/v1/games")), std::string("steam-570")));
  CHECK(std::ranges::contains(ids(Get(client, "/v1/games?include_off=true")), std::string("steam-570")));
}

TEST_CASE("An unknown source can't be planned or removed") {
  LiveServer server(TempDir("sources-unknown"));
  httplib::Client client = server.Client();
  auto plan = client.Get("/v1/sources/nope/removal");
  REQUIRE(plan != nullptr);
  CHECK(plan->status == 404);
  auto removed = client.Post("/v1/sources/nope/remove");
  REQUIRE(removed != nullptr);
  CHECK(removed->status == 404);
}

TEST_CASE("Microsoft 365's apps are asked for by name, and only once Office is being set up or is") {
  LiveServer server(TempDir("office-apps"));
  httplib::Client client = server.Client();
  // Listed before Office is installed, so they can be picked first.
  const json launchers = Get(client, "/v1/launchers");
  const auto office = std::ranges::find(launchers, json("office"), [](const json& l) { return l["id"]; });
  REQUIRE(office != launchers.end());
  CHECK((*office)["installed"] == false);
  CHECK(std::ranges::contains((*office)["apps"], json{{"ref", "word"}, {"name", "Word"}}));

  const auto post = [&](const json& body) {
    auto res = client.Post("/v1/launchers/office/apps", body.dump(), "application/json");
    REQUIRE(res != nullptr);
    return std::pair{res->status, json::parse(res->body)};
  };
  const auto [unknown, unknown_body] = post({{"apps", {"word", "notepad"}}});
  CHECK(unknown == 400);
  CHECK(unknown_body["error"]["code"] == "unknown_app");
  CHECK(post({{"apps", json::array()}}).first == 400);
  CHECK(post({{"apps", {1}}}).first == 400);

  const auto [missing, missing_body] = post({{"apps", {"word", "excel"}}});
  CHECK(missing == 409);
  CHECK(missing_body["error"]["code"] == "launcher_not_installed");
}

TEST_CASE("Copied text is searched for each store's credential, and other text finds none") {
  LiveServer server(TempDir("store-find-credential"));
  httplib::Client client = server.Client();
  const auto find = [&](const std::string& store, const std::string& text) {
    auto res = client.Post("/v1/stores/" + store + "/login/find", json{{"text", text}}.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    return json::parse(res->body)["credential"];
  };
  const std::string epic_code = "7c1e05d8a3f94b6e9d2a41c0b8f3e5a7";
  CHECK(find("epic", R"({"redirectUrl":"https://localhost","authorizationCode":")" + epic_code + R"(","sid":null})") ==
        epic_code);
  // Firefox's JSON viewer, all of it selected and copied.
  CHECK(find("epic", "warning\t\"Do not share this code\"\nauthorizationCode\t\"" + epic_code + "\"\nsid\tnull") ==
        epic_code);
  CHECK(find("epic", "  " + epic_code + "\n") == epic_code);
  CHECK(find("epic", "https://www.epicgames.com/id/login").is_null());

  CHECK(find("gog", "https://embed.gog.com/on_login_success?origin=client&code=Kx9pR2vQ8mWZ&x=1") == "Kx9pR2vQ8mWZ");
  CHECK(find("gog", "Kx9pR2vQ8mWZ").is_null());

  CHECK(find("amazon", "https://www.amazon.com/?openid.oa2.authorization_code=ANbXqzKp&openid.mode=id_res") ==
        "ANbXqzKp");
  CHECK(find("amazon", "https://www.amazon.com/ap/signin").is_null());

  CHECK(find("itch", " a7Xp3QnR9vLm2KwT8bYza7Xp3QnR9vLm2KwT8bYz ") == "a7Xp3QnR9vLm2KwT8bYza7Xp3QnR9vLm2KwT8bYz");
  CHECK(find("itch", "Generate new API key").is_null());

  const std::string cookie = R"("eyJpZCI6IjY4NDE1OTI3NyJ9|1791245011|a3f9c2d4e5")";
  CHECK(find("humble", cookie + "\n") == cookie);
  CHECK(find("humble", "csrf_cookie").is_null());

  auto unknown = client.Post("/v1/stores/nope/login/find", json{{"text", "x"}}.dump(), "application/json");
  REQUIRE(unknown != nullptr);
  CHECK(unknown->status == 404);
}

TEST_CASE("Steam accounts on this computer list the most recent first, and the chosen one is selected") {
  LiveServer server(TempDir("steam-accounts"));
  const fs::path steam = server.config().GetPath("steam.root");
  httplib::Client client = server.Client();
  fs::create_directories(steam / "steamapps");
  Touch(steam / "config" / "loginusers.vdf",
        R"("users" { "76561198000000001" { "AccountName" "older" "PersonaName" "Old" "Timestamp" "100" }
                     "76561198000000002" { "AccountName" "newer" "PersonaName" "New" "Timestamp" "300" }
                     "76561198000000003" { "AccountName" "main" "PersonaName" "Main" "MostRecent" "1" "Timestamp" "200" } })");
  const auto accounts = [&] {
    auto res = client.Get("/v1/steam/accounts");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    return json::parse(res->body);
  };

  const json listed = accounts();
  REQUIRE(listed["accounts"].size() == 3);
  CHECK(listed["accounts"][0]["account_name"] == "main");
  CHECK(listed["accounts"][0]["persona_name"] == "Main");
  CHECK(listed["accounts"][1]["account_name"] == "newer");
  CHECK(listed["accounts"][2]["account_name"] == "older");
  CHECK(listed["found"].get<bool>());
  CHECK(listed["selected"] == "76561198000000003");

  REQUIRE(server.MutableConfig().Set("steam.steamid64", "76561198000000001"));
  CHECK(accounts()["selected"] == "76561198000000001");
}

TEST_CASE("Steam's installed games are listed most recently played first without being added") {
  LiveServer server(TempDir("steam-installed"));
  const fs::path steam = server.config().GetPath("steam.root");
  httplib::Client client = server.Client();
  for (const auto& [appid, name] : {std::pair{"400", "Portal"}, {"620", "Portal 2"}, {"70", "Half-Life"}}) {
    WriteManifest(steam, appid, name, name);
    fs::create_directories(steam / "steamapps" / "common" / name);
  }
  Touch(steam / "config" / "loginusers.vdf",
        R"("users" { "76561198000000001" { "AccountName" "main" "MostRecent" "1" } })");
  Touch(steam / "userdata" / "39734273" / "config" / "localconfig.vdf",
        R"("UserLocalConfigStore" { "Software" { "Valve" { "Steam" { "apps" {
             "400" { "LastPlayed" "1000" } "620" { "LastPlayed" "3000" } } } } } })");

  const json listed = Get(client, "/v1/steam/installed");
  CHECK(listed["found"].get<bool>());
  REQUIRE(listed["games"].size() == 3);
  CHECK(listed["games"][0]["name"] == "Portal 2");
  CHECK(listed["games"][0]["last_played_at"] == 3000);
  CHECK(listed["games"][1]["appid"] == "400");
  CHECK(listed["games"][2]["appid"] == "70");
  CHECK_FALSE(listed["games"][2].contains("last_played_at"));
  CHECK(Get(client, "/v1/games").empty());
}

TEST_CASE("Settings from before the sources table carry over to it") {
  test::TestEnv env("sources-import");
  Touch(env.dir / "settings.toml", "[gog]\nenabled = false\n");
  env.config.Load();
  test::Isolate(env.config);
  env.config.SetFrontendSettings(json{{"hidden_sources", json::array({"epic"})},
                                      {"source_order", json::array({"gog", "steam"})}});

  migrate::ImportSourceState(env.config, env.games);

  CHECK_FALSE(env.games.Source("gog").enabled);
  CHECK_FALSE(env.games.Source("epic").in_sidebar);
  std::vector<std::string> order;
  for (const store::SourceState& source : env.games.Sources()) order.push_back(source.id);
  const auto gog = std::find(order.begin(), order.end(), "gog");
  const auto steam = std::find(order.begin(), order.end(), "steam");
  REQUIRE(gog != order.end());
  REQUIRE(steam != order.end());
  CHECK(gog < steam);
}
