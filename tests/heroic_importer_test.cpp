#include <doctest.h>

#include <filesystem>
#include <format>

#include "heroic/HeroicImporter.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

// A Heroic config folder with one installed game from each of its stores, laid out as Heroic
// writes them, plus a macOS game and a browser game it can't run.
struct HeroicFixture {
  test::TestEnv env;
  fs::path heroic;
  fs::path games;

  HeroicFixture() : env("heroic-import") {
    heroic = env.config.GetPath("heroic.config_dir");
    games = env.dir / "Games";
    test::Touch(heroic / "config.json", "{}");

    test::Touch(games / "Hollow" / "Hollow.exe");
    test::Touch(games / "MacOnly" / "Mac.app");
    test::Touch(heroic / "legendaryConfig" / "legendary" / "installed.json",
                std::format(R"({{
  "Hollow": {{"title": "Hollow Path", "install_path": "{0}/Hollow", "executable": "Hollow.exe",
              "platform": "Windows", "is_dlc": false, "launch_parameters": "-skipintro"}},
  "HollowDLC": {{"title": "Hollow Path Soundtrack", "install_path": "{0}/Hollow",
                 "executable": "", "platform": "Windows", "is_dlc": true}},
  "MacOnly": {{"title": "Mac Only", "install_path": "{0}/MacOnly", "executable": "Mac.app",
               "platform": "Mac", "is_dlc": false}}
}})",
                            games.string()));
    test::Touch(heroic / "GamesConfig" / "Hollow.json",
                std::format(R"({{"Hollow": {{"winePrefix": "{}"}}}})",
                            (env.dir / "Prefixes" / "Hollow").string()));
    // A Proton prefix keeps its drive_c under pfx/.
    fs::create_directories(env.dir / "Prefixes" / "Hollow" / "pfx" / "drive_c");

    test::Touch(games / "Tunic" / "bin" / "Tunic.exe");
    test::Touch(games / "Tunic" / "goggame-1207.info",
                R"({"playTasks": [{"isPrimary": false, "path": "Manual.pdf"},
                                  {"isPrimary": true, "path": "bin/Tunic.exe", "arguments": "-w"}]})");
    test::Touch(games / "Linuxy" / "start.sh", "", true);
    test::Touch(heroic / "gog_store" / "installed.json", std::format(R"({{"installed": [
  {{"appName": "1207", "install_path": "{0}/Tunic", "platform": "windows", "is_dlc": false}},
  {{"appName": "1300", "install_path": "{0}/Linuxy", "platform": "linux", "is_dlc": false}},
  {{"appName": "1400", "platform": "linux", "is_dlc": false}}
]}})",
                                                                     games.string()));
    test::Touch(heroic / "gog_store" / "library.json",
                R"({"games": [{"app_name": "1207", "title": "Tunic"},
                              {"app_name": "1300", "title": "Linuxy"}]})");
    test::Touch(heroic / "GamesConfig" / "1207.json",
                std::format(R"({{"1207": {{"winePrefix": "{}"}}}})",
                            (env.dir / "Prefixes" / "Tunic").string()));
    fs::create_directories(env.dir / "Prefixes" / "Tunic" / "drive_c");

    test::Touch(games / "Lost" / "Lost.exe");
    test::Touch(games / "Lost" / "fuel.json",
                R"({"Main": {"Command": "Lost.exe", "Args": ["--no-launcher", "two words"]}})");
    test::Touch(heroic / "nile_config" / "nile" / "installed.json",
                std::format(R"([{{"id": "amzn1.lost", "path": "{}/Lost"}}])", games.string()));
    test::Touch(heroic / "nile_config" / "nile" / "library.json",
                R"([{"id": "amzn1.lost", "product": {"title": "Lost Lands"}}])");
    // No GamesConfig for the Amazon game: Heroic hasn't made its prefix yet.

    test::Touch(games / "Sided" / "Sided.sh", "", true);
    test::Touch(heroic / "sideload_apps" / "library.json", std::format(R"({{"games": [
  {{"app_name": "sd1", "title": "Sided", "install": {{"executable": "{}/Sided/Sided.sh",
                                                      "platform": "linux"}}}},
  {{"app_name": "sd2", "title": "Web Toy", "install": {{"executable": "https://example.com",
                                                        "platform": "Browser"}}}}
]}})",
                                                                       games.string()));
  }

  Result<heroic::HeroicImportSummary> Import() {
    return heroic::HeroicImporter(env.config, env.games, env.events).Import();
  }

  std::optional<model::Game> FindRef(const std::string& ref) {
    for (const model::Game& game : env.games.All())
      if (game.source == "heroic" && game.source_ref == ref) return game;
    return std::nullopt;
  }
};

}  // namespace

TEST_CASE("HeroicImporter adds the installed games of every Heroic store, with their prefixes") {
  HeroicFixture fx;
  const auto summary = fx.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 4);
  CHECK(summary->unsupported == 2);  // the macOS game and the browser game
  CHECK(summary->incomplete ==
        2);  // the Amazon game without a prefix, the GOG one without a folder
  CHECK_FALSE(fx.FindRef("gog:1400").has_value());

  const auto epic = fx.FindRef("epic:Hollow");
  REQUIRE(epic.has_value());
  CHECK(epic->name == "Hollow Path");
  CHECK(epic->install_path == (fx.games / "Hollow").string());
  CHECK(epic->exe_path == "Hollow.exe");
  CHECK(epic->args == "-skipintro");
  CHECK(epic->data_dir == (fx.env.dir / "Prefixes" / "Hollow" / "pfx").string());
  CHECK(epic->platform == model::Platform::Windows);
  CHECK(epic->status == model::GameStatus::Ready);

  const auto gog = fx.FindRef("gog:1207");
  REQUIRE(gog.has_value());
  CHECK(gog->name == "Tunic");
  CHECK(gog->install_path == (fx.games / "Tunic" / "bin").string());
  CHECK(gog->exe_path == "Tunic.exe");
  CHECK(gog->args == "-w");
  CHECK(gog->data_dir == (fx.env.dir / "Prefixes" / "Tunic").string());

  const auto linux_gog = fx.FindRef("gog:1300");
  REQUIRE(linux_gog.has_value());
  CHECK(linux_gog->exe_path == "start.sh");
  CHECK(linux_gog->platform == model::Platform::Native);
  CHECK(linux_gog->data_dir.empty());

  const auto sideloaded = fx.FindRef("sideload:sd1");
  REQUIRE(sideloaded.has_value());
  CHECK(sideloaded->name == "Sided");
  CHECK(sideloaded->platform == model::Platform::Native);

  CHECK_FALSE(fx.FindRef("epic:HollowDLC").has_value());
}

TEST_CASE("HeroicImporter imports Amazon games with their fuel.json arguments") {
  HeroicFixture fx;
  test::Touch(fx.heroic / "GamesConfig" / "amzn1.lost.json",
              std::format(R"({{"amzn1.lost": {{"winePrefix": "{}"}}}})",
                          (fx.env.dir / "Prefixes" / "Lost").string()));
  REQUIRE(fx.Import().has_value());
  const auto amazon = fx.FindRef("amazon:amzn1.lost");
  REQUIRE(amazon.has_value());
  CHECK(amazon->name == "Lost Lands");
  CHECK(amazon->exe_path == "Lost.exe");
  CHECK(amazon->args == R"(--no-launcher "two words")");
}

TEST_CASE("HeroicImporter's re-import keeps what was changed in Mira and follows Heroic's moves") {
  HeroicFixture fx;
  REQUIRE(fx.Import().has_value());
  auto epic = fx.FindRef("epic:Hollow");
  REQUIRE(epic.has_value());
  const std::string id = epic->id;

  // Arguments and prefix changed in Mira stay; the arguments of an untouched game follow Heroic.
  epic->args = "-mine";
  const fs::path own_prefix = fx.env.dir / "MyPrefix";
  fs::create_directories(own_prefix);
  epic->data_dir = own_prefix.string();
  REQUIRE(fx.env.games.Upsert(*epic).has_value());
  test::Touch(fx.heroic / "GamesConfig" / "1207.json",
              std::format(R"({{"1207": {{"winePrefix": "{}"}}}})",
                          (fx.env.dir / "Prefixes" / "Tunic2").string()));

  const auto summary = fx.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 0);
  CHECK(summary->updated == 4);

  epic = fx.FindRef("epic:Hollow");
  REQUIRE(epic.has_value());
  CHECK(epic->id == id);
  CHECK(epic->args == "-mine");
  CHECK(epic->data_dir == own_prefix.string());
  // The old prefix still exists, so Mira keeps it.
  CHECK(fx.FindRef("gog:1207")->data_dir == (fx.env.dir / "Prefixes" / "Tunic").string());

  // A game moved in Heroic, whose old program is gone, comes back at its new folder.
  fs::rename(fx.games / "Hollow", fx.games / "Hollow2");
  test::Touch(fx.heroic / "legendaryConfig" / "legendary" / "installed.json",
              std::format(R"({{"Hollow": {{"title": "Hollow Path", "install_path": "{}/Hollow2",
                                           "executable": "Hollow.exe", "platform": "Windows"}}}})",
                          fx.games.string()));
  REQUIRE(fx.Import().has_value());
  epic = fx.FindRef("epic:Hollow");
  REQUIRE(epic.has_value());
  CHECK(epic->id == id);
  CHECK(epic->install_path == (fx.games / "Hollow2").string());
}

TEST_CASE("HeroicImporter leaves a folder another store source installed to that source") {
  HeroicFixture fx;
  model::Game owned;
  owned.id = "tunic";
  owned.name = "Tunic";
  owned.install_path = (fx.games / "Tunic" / "bin").string();
  owned.source = "gog";
  owned.exe_path = "Tunic.exe";
  REQUIRE(fx.env.games.Upsert(owned).has_value());

  REQUIRE(fx.Import().has_value());
  CHECK_FALSE(fx.FindRef("gog:1207").has_value());
  CHECK(fx.env.games.Find("tunic")->source == "gog");
}

TEST_CASE("HeroicImporter reports Heroic missing, with the setting that points to it") {
  test::TestEnv env("heroic-missing");
  const auto result = heroic::HeroicImporter(env.config, env.games, env.events).Import();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == "heroic_not_found");
}
