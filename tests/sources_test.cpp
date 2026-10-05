#include <doctest.h>

#include <filesystem>
#include <string>

#include <json.hpp>

#include "gog/Gog.h"
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
  CHECK_FALSE(server.config().GetBool("steam.enabled"));
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
  CHECK_FALSE(server.config().GetBool("gog.enabled"));
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
  CHECK(trackmania["tags"] == json::array({"ubisoft"}));

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
