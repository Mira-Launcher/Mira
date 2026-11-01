#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <sstream>

#include "runner/Exec.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string Read(const fs::path& file) {
  std::ifstream in(file);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

// The folder games are installed in: on this computer, and as the prefix sees it.
struct GamesDir {
  fs::path host;
  std::string windows;
};
GamesDir InPrefix(const fs::path& prefix) {
  return {prefix / "drive_c/Games", "C:\\Games"};
}
// A library folder outside the prefix, which Wine reaches through its Z: drive.
GamesDir Outside(const fs::path& prefix, const fs::path& library) {
  fs::create_directories(prefix / "dosdevices");
  fs::create_directory_symlink("/", prefix / "dosdevices/z:");
  std::string windows = "Z:" + library.string();
  std::ranges::replace(windows, '/', '\\');
  return {library, windows};
}

// A Windows game installed in `games`, with `manager` as a launch option. `data_dir` is the game's
// prefix folder.
model::Game WindowsGame(const std::string& id, const std::string& name, const std::string& manager,
                        const fs::path& data_dir, const GamesDir& games) {
  model::Game game;
  game.id = id;
  game.name = name;
  game.platform = model::Platform::Windows;
  game.status = model::GameStatus::Ready;
  game.data_dir = data_dir.string();
  game.install_path = (games.host / name).string();
  game.exe_path = "game.exe";
  test::Touch(fs::path(game.install_path) / "game.exe");
  game.launch = {{"options", json::array({{{"id", manager},
                                           {"name", manager},
                                           {"mode", "tool"},
                                           {"exe", "/nowhere/manager.exe"},
                                           {"native", false}}})}};
  return game;
}

json Summary(httplib::Client& client, const std::string& game, const std::string& manager) {
  const auto got = client.Get("/v1/games/" + game + "/mod-managers/" + manager);
  REQUIRE(got != nullptr);
  REQUIRE(got->status == 200);
  return json::parse(got->body);
}

json Remove(httplib::Client& client, const std::string& game, const std::string& manager) {
  return test::AwaitJob(client, client.Delete("/v1/games/" + game + "/mod-managers/" + manager));
}

bool HasOption(test::LiveServer& server, const std::string& game, const std::string& option) {
  const auto found = server.games().Find(game);
  REQUIRE(found.has_value());
  for (const json& entry : found->launch.value("options", json::array()))
    if (entry.value("id", std::string()) == option) return true;
  return false;
}

// Vortex's state for game `name` in `games` (Vortex's id `vortex_id`) in `prefix`, as it leaves it
// after deploying a mod by hard links into <game>\Data and another by a symlink into the game's
// folder.
struct VortexGame {
  fs::path data;     // the game's Data folder
  fs::path vortex;   // %APPDATA%\Vortex
  fs::path staging;  // %APPDATA%\Vortex\<id>\mods
};
VortexGame DeployVortex(const fs::path& prefix, const GamesDir& games, const std::string& name,
                        const std::string& vortex_id) {
  const fs::path install = games.host / name;
  const std::string windows_install = games.windows + "\\" + name;
  const fs::path vortex = prefix / "drive_c/users/steamuser/AppData/Roaming/Vortex";
  const fs::path staging = vortex / vortex_id / "mods";
  const fs::path data = install / "Data";
  test::Touch(staging / "__vortex_staging_folder");
  test::Touch(staging / "ModA/a.esp", "mod a");
  test::Touch(staging / "ModA/textures/b.dds", "mod b");
  test::Touch(staging / "ModA/c.esm", "mod c");
  test::Touch(staging / "ModA/d.esp", "mod d");
  test::Touch(staging / "ModB/dinput8.dll", "mod dll");
  test::Touch(vortex / "downloads" / vortex_id / "ModA.zip", "zip");
  // The game's own files: c.esm was set aside for the mod's, d.esp was replaced by the user since.
  test::Touch(data / "own.esp", "the game's");
  test::Touch(data / "c.esm.vortex_backup", "the game's c");
  test::Touch(data / "d.esp", "the user's d");
  fs::create_hard_link(staging / "ModA/a.esp", data / "a.esp");
  fs::create_directories(data / "textures");
  test::Touch(data / "textures/__folder_managed_by_vortex");
  fs::create_hard_link(staging / "ModA/textures/b.dds", data / "textures/b.dds");
  fs::create_hard_link(staging / "ModA/c.esm", data / "c.esm");
  fs::create_symlink(staging / "ModB/dinput8.dll", install / "dinput8.dll");
  // A symlink of the user's that points elsewhere.
  test::Touch(prefix / "elsewhere.dll");
  fs::create_symlink(prefix / "elsewhere.dll", install / "elsewhere.dll");

  const std::string windows_staging =
      "C:\\users\\steamuser\\AppData\\Roaming\\Vortex\\" + vortex_id + "\\mods";
  const json hardlinks = {{"gameId", vortex_id},
                          {"deploymentMethod", "hardlink"},
                          {"targetPath", windows_install + "\\Data"},
                          {"stagingPath", windows_staging},
                          {"files", json::array({
                                        {{"relPath", "a.esp"}, {"source", "ModA"}},
                                        {{"relPath", "textures\\b.dds"}, {"source", "ModA"}},
                                        {{"relPath", "c.esm"}, {"source", "ModA"}},
                                        {{"relPath", "d.esp"}, {"source", "ModA"}},
                                    })}};
  const json symlinks = {{"gameId", vortex_id},
                         {"deploymentMethod", "symlink"},
                         {"targetPath", windows_install},
                         {"stagingPath", windows_staging},
                         {"files", json::array({
                                       {{"relPath", "dinput8.dll"}, {"source", "ModB"}},
                                       {{"relPath", "elsewhere.dll"}, {"source", "ModB"}},
                                   })}};
  test::Touch(data / "vortex.deployment.json", hardlinks.dump());
  test::Touch(staging / "vortex.deployment.json", hardlinks.dump());
  test::Touch(install / "vortex.deployment.dinput.json", symlinks.dump());
  test::Touch(staging / "vortex.deployment.dinput.json", symlinks.dump());
  return {data, vortex, staging};
}

bool SystemdUserAvailable() {
  Command probe;
  probe.argv = {"systemd-run", "--user", "--scope", "--quiet", "--collect", "--", "true"};
  auto ran = runner::RunAndWait(probe);
  return ran.has_value() && ran->exit_code == 0;
}

}  // namespace

TEST_CASE("Removing Vortex from a game takes out what it deployed and puts the game's files back") {
  test::LiveServer server(test::TempDir("purge-vortex"));
  const fs::path data_dir = server.config().File().parent_path() / "prefix";
  fs::path prefix = data_dir;
  bool shared = false;
  bool outside = false;
  SUBCASE("in the game's own prefix, all of Vortex's folder goes") {}
  SUBCASE("in umu's layout, with the prefix in pfx/") {
    prefix = data_dir / "pfx";
  }
  SUBCASE("installed in a library folder outside the prefix") {
    outside = true;
  }
  SUBCASE("in a prefix another game shares, only this game's part of it") {
    shared = true;
  }
  fs::create_directories(prefix / "drive_c/users/steamuser");
  const GamesDir games =
      outside ? Outside(prefix, data_dir.parent_path() / "library") : InPrefix(prefix);
  const VortexGame fir = DeployVortex(prefix, games, "Fir", "fir");
  REQUIRE(server.games().Upsert(WindowsGame("fir", "Fir", "vortex", data_dir, games)).has_value());
  if (shared) {
    DeployVortex(prefix, games, "Elm", "elm");
    REQUIRE(
        server.games().Upsert(WindowsGame("elm", "Elm", "vortex", data_dir, games)).has_value());
  }
  const fs::path install = fir.data.parent_path();
  test::Touch(fir.vortex / "state.v2/LOCK");
  auto client = server.Client();

  const json summary = Summary(client, "fir", "vortex");
  CHECK(summary["deployed"] == 4);  // a.esp, textures/b.dds, c.esm, dinput8.dll
  CHECK(summary["restored"] == 1);
  CHECK(summary["open"] == false);
  CHECK(summary["bytes"].get<std::uint64_t>() > 0);
  json paths = json::array();
  for (const json& folder : summary["deleted"]) paths.push_back(folder["path"]);
  if (shared) {
    CHECK(paths ==
          json::array({(fir.vortex / "fir").string(), (fir.vortex / "downloads/fir").string()}));
  } else {
    CHECK(paths == json::array({fir.vortex.string()}));
  }
  // Asking deletes nothing.
  CHECK(fs::exists(fir.data / "a.esp"));

  const json job = Remove(client, "fir", "vortex");
  REQUIRE(job["state"] == "finished");
  CHECK_FALSE(HasOption(server, "fir", "vortex"));
  CHECK_FALSE(fs::exists(fir.data / "a.esp"));
  CHECK_FALSE(fs::exists(fir.data / "textures"));
  CHECK_FALSE(fs::exists(install / "dinput8.dll"));
  CHECK_FALSE(fs::exists(fir.data / "vortex.deployment.json"));
  CHECK_FALSE(fs::exists(install / "vortex.deployment.dinput.json"));
  CHECK(Read(fir.data / "c.esm") == "the game's c");
  CHECK_FALSE(fs::exists(fir.data / "c.esm.vortex_backup"));
  // What isn't Vortex's stays.
  CHECK(Read(fir.data / "own.esp") == "the game's");
  CHECK(Read(fir.data / "d.esp") == "the user's d");
  CHECK(fs::is_symlink(install / "elsewhere.dll"));
  CHECK(fs::exists(install / "game.exe"));
  CHECK_FALSE(fs::exists(fir.vortex / "fir"));
  CHECK_FALSE(fs::exists(fir.vortex / "downloads/fir"));
  if (shared) {
    // Elm's mods and Vortex's own settings are untouched.
    CHECK(fs::exists(fir.vortex / "state.v2/LOCK"));
    CHECK(fs::exists(fir.vortex / "elm/mods/ModA/a.esp"));
    CHECK(fs::exists(fir.vortex / "downloads/elm/ModA.zip"));
    CHECK(fs::exists(games.host / "Elm/Data/a.esp"));
    CHECK(HasOption(server, "elm", "vortex"));
  } else {
    CHECK_FALSE(fs::exists(fir.vortex));
  }
}

TEST_CASE("Removing Vortex refuses while it's open for the game, and deletes nothing") {
  if (!SystemdUserAvailable()) {
    MESSAGE("no systemd user session; skipped");
    return;
  }
  test::LiveServer server(test::TempDir("purge-vortex-open"));
  const fs::path dir = server.config().File().parent_path();
  const fs::path prefix = dir / "prefix";
  fs::create_directories(prefix / "drive_c/users/steamuser");
  const VortexGame fir = DeployVortex(prefix, InPrefix(prefix), "Fir", "fir");
  model::Game game = WindowsGame("fir", "Fir", "vortex", prefix, InPrefix(prefix));
  // Stands in for Vortex: a native tool that stays open.
  test::Touch(dir / "vortex.sh", "#!/bin/sh\nexec sleep 300\n", /*executable=*/true);
  game.launch["options"][0]["exe"] = (dir / "vortex.sh").string();
  game.launch["options"][0]["native"] = true;
  REQUIRE(server.games().Upsert(game).has_value());
  auto client = server.Client();
  const auto opened =
      client.Post("/v1/games/fir/launch", R"({"option": "vortex"})", "application/json");
  REQUIRE(opened != nullptr);
  REQUIRE(json::parse(opened->body)["status"] == "opened");

  CHECK(Summary(client, "fir", "vortex")["open"] == true);
  const auto refused = client.Delete("/v1/games/fir/mod-managers/vortex");
  REQUIRE(refused != nullptr);
  CHECK(refused->status == 409);
  const json error = json::parse(refused->body)["error"];
  CHECK(error["code"] == "mod_manager_open");
  CHECK_FALSE(error["hint"].get<std::string>().empty());
  CHECK(fs::exists(fir.data / "a.esp"));
  CHECK(fs::exists(fir.vortex));
  CHECK(HasOption(server, "fir", "vortex"));
  // Nor does removing its source.
  const auto source = client.Post("/v1/sources/vortex/remove", "{}", "application/json");
  REQUIRE(source != nullptr);
  CHECK(source->status == 409);
  CHECK(json::parse(source->body)["error"]["code"] == "mod_manager_open");
  CHECK(fs::exists(fir.data / "a.esp"));

  client.Post("/v1/games/fir/stop");
  CHECK(test::WaitUntil([&] { return server.services().supervisor.Tools("fir").empty(); }));
}

TEST_CASE("Removing Vortex as a source purges it from every game it was added to") {
  test::LiveServer server(test::TempDir("purge-vortex-source"));
  const fs::path dir = server.config().File().parent_path();
  test::EnvVar config_home("XDG_CONFIG_HOME", (dir / "xdg").c_str());
  const fs::path program = dir / "xdg/mira/tools/vortex";
  test::Touch(program / "Vortex.exe");
  std::vector<VortexGame> deployed;
  for (const std::string name : {"Fir", "Elm"}) {
    const fs::path prefix = dir / ("prefix-" + name);
    fs::create_directories(prefix / "drive_c/users/steamuser");
    std::string id = name;
    std::ranges::transform(id, id.begin(), [](unsigned char c) { return std::tolower(c); });
    deployed.push_back(DeployVortex(prefix, InPrefix(prefix), name, id));
    REQUIRE(server.games()
                .Upsert(WindowsGame(id, name, "vortex", prefix, InPrefix(prefix)))
                .has_value());
  }
  auto client = server.Client();

  const auto plan = client.Get("/v1/sources/vortex/removal");
  REQUIRE(plan != nullptr);
  json purged = json::array();
  const json removal = json::parse(plan->body);
  for (const json& game : removal["purged"]) purged.push_back(game["id"]);
  CHECK(purged.size() == 2);
  CHECK(std::ranges::contains(purged, json("fir")));
  CHECK(std::ranges::contains(purged, json("elm")));

  const json job =
      test::AwaitJob(client, client.Post("/v1/sources/vortex/remove", "{}", "application/json"));
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["problems"].empty());
  CHECK_FALSE(fs::exists(program));
  for (const VortexGame& game : deployed) {
    const fs::path install = game.data.parent_path();
    CHECK_FALSE(fs::exists(game.data / "a.esp"));
    CHECK_FALSE(fs::exists(install / "dinput8.dll"));
    CHECK(Read(game.data / "c.esm") == "the game's c");
    CHECK(Read(game.data / "own.esp") == "the game's");
    CHECK_FALSE(fs::exists(game.vortex));
  }
  CHECK_FALSE(HasOption(server, "fir", "vortex"));
  CHECK_FALSE(HasOption(server, "elm", "vortex"));
}

TEST_CASE("Removing Mod Organizer 2 from a game deletes that game's instance only") {
  test::LiveServer server(test::TempDir("purge-mo2"));
  const fs::path dir = server.config().File().parent_path();
  // Mira's copy of MO2 lives under the config folder.
  test::EnvVar config_home("XDG_CONFIG_HOME", (dir / "xdg").c_str());
  const fs::path prefix = dir / "prefix";
  const fs::path instances = prefix / "drive_c/users/steamuser/AppData/Local/ModOrganizer";
  test::Touch(instances / "Fir/ModOrganizer.ini",
              "[General]\r\ngameName=Fir\r\ngamePath=@ByteArray(C:\\\\Games\\\\Fir)\r\n");
  test::Touch(instances / "Fir/mods/ModA/a.esp", "mod a");
  test::Touch(instances / "Fir/downloads/ModA.zip", "zip");
  test::Touch(instances / "Elm/ModOrganizer.ini",
              "[General]\ngamePath=@ByteArray(C:\\\\Games\\\\Elm)\n");
  test::Touch(instances / "Elm/mods/ModB/b.esp", "mod b");
  bool portable = false;
  SUBCASE("in its own instance folder") {}
  SUBCASE("with a portable instance in Mira's copy") {
    portable = true;
    const fs::path own = dir / "xdg/mira/tools/mo2";
    test::Touch(own / "ModOrganizer.exe");
    test::Touch(own / "portable.txt");
    test::Touch(own / "ModOrganizer.ini", "[General]\ngamePath=@ByteArray(C:\\\\Games\\\\Fir)\n");
    test::Touch(own / "mods/ModC/c.esp", "mod c");
  }
  REQUIRE(server.games()
              .Upsert(WindowsGame("fir", "Fir", "mo2", prefix, InPrefix(prefix)))
              .has_value());
  auto client = server.Client();

  const json summary = Summary(client, "fir", "mo2");
  CHECK(summary["deployed"] == 0);
  CHECK(summary["deleted"][0]["path"] == (instances / "Fir").string());
  CHECK(summary["deleted"].size() == (portable ? 3 : 1));  // with the portable ini and mods

  REQUIRE(Remove(client, "fir", "mo2")["state"] == "finished");
  CHECK_FALSE(HasOption(server, "fir", "mo2"));
  CHECK_FALSE(fs::exists(instances / "Fir"));
  CHECK(fs::exists(instances / "Elm/mods/ModB/b.esp"));
  CHECK(fs::exists(prefix / "drive_c/Games/Fir/game.exe"));
  if (portable) {
    const fs::path own = dir / "xdg/mira/tools/mo2";
    CHECK_FALSE(fs::exists(own / "mods"));
    CHECK_FALSE(fs::exists(own / "ModOrganizer.ini"));
    CHECK(fs::exists(own / "ModOrganizer.exe"));
  }
}
