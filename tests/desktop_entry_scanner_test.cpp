#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>

#include <json.hpp>

#include "api/EventBus.h"
#include "config/Config.h"
#include "desktop/DesktopEntryScanner.h"
#include "store/GameStore.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

// The scanner also walks $XDG_DATA_HOME/applications, $XDG_DATA_DIRS, and
// the two well-known Flatpak export dirs unconditionally -- on a real
// desktop machine those pick up real installed apps, which would make a
// test's expected candidate count depend on whatever happens to be
// installed. Point both env vars at empty, otherwise-unused directories so
// only this fixture's extra_dirs entry is ever actually scanned.
// PATH is a fixture dir holding only the commands the entries name, so
// resolving them doesn't depend on what the machine has installed.
// All three are put back afterwards, so later tests see the real environment.
struct Fixture {
  fs::path apps_dir;
  fs::path state_dir;
  fs::path empty_data_home;
  fs::path bin_dir;
  std::map<std::string, std::optional<std::string>> saved_env;
  config::Config config;
  store::GameStore games;
  api::EventBus events;

  explicit Fixture(const char* name)
      : apps_dir(TempDir((std::string(name) + "-apps").c_str())),
        state_dir(TempDir((std::string(name) + "-state").c_str())),
        empty_data_home(TempDir((std::string(name) + "-empty-data-home").c_str())),
        bin_dir(TempDir((std::string(name) + "-bin").c_str())),
        config(state_dir / "settings.toml"),
        games(state_dir / "games.toml") {
    for (const char* var : {"XDG_DATA_HOME", "XDG_DATA_DIRS", "PATH"}) {
      const char* value = std::getenv(var);
      saved_env[var] = value != nullptr ? std::optional<std::string>(value) : std::nullopt;
    }
    setenv("XDG_DATA_HOME", empty_data_home.string().c_str(), 1);
    setenv("XDG_DATA_DIRS", empty_data_home.string().c_str(), 1);
    setenv("PATH", bin_dir.c_str(), 1);
    for (const char* command : {"flatpak", "first-game", "second-game", "app-game"}) {
      test::Touch(bin_dir / command, "#!/bin/sh\n", /*executable=*/true);
    }
    config.Load();
    REQUIRE(config.Set("desktop_import.extra_dirs", nlohmann::json::array({apps_dir.string()})).has_value());
    games.Load();
  }

  ~Fixture() {
    for (const auto& [var, value] : saved_env) {
      if (value) {
        setenv(var.c_str(), value->c_str(), 1);
      } else {
        unsetenv(var.c_str());
      }
    }
  }
};

}  // namespace

TEST_CASE("DesktopEntryScanner: ListCandidates finds a Flatpak entry and a plain native entry, "
          "skips Mira's own, Steam's, NoDisplay, and non-Application entries") {
  Fixture fx("desktop-scan-candidates");

  test::Touch(fx.apps_dir / "com.example.App.desktop",
              "[Desktop Entry]\n"
              "Type=Application\n"
              "Name=Example App\n"
              "Icon=com.example.App\n"
              "Exec=flatpak run --branch=stable --command=example com.example.App @@u %u @@\n"
              "X-Flatpak=com.example.App\n");

  test::Touch(fx.apps_dir / "native-game.desktop",
              "[Desktop Entry]\n"
              "Type=Application\n"
              "Name=Native Game\n"
              "Exec=\"/opt/nativegame/game\" --fullscreen\n");

  test::Touch(fx.apps_dir / "mira-celeste.desktop",
              "[Desktop Entry]\n"
              "Type=Application\n"
              "Name=Celeste\n"
              "Exec=mira launch celeste\n"
              "X-Mira-Game-Id=celeste\n");

  test::Touch(fx.apps_dir / "steam-game.desktop",
              "[Desktop Entry]\n"
              "Type=Application\n"
              "Name=Some Steam Game\n"
              "Exec=steam steam://rungameid/12345\n");

  test::Touch(fx.apps_dir / "hidden.desktop",
              "[Desktop Entry]\n"
              "Type=Application\n"
              "Name=Hidden Thing\n"
              "Exec=/usr/bin/hiddenthing\n"
              "NoDisplay=true\n");

  test::Touch(fx.apps_dir / "not-an-app.desktop",
              "[Desktop Entry]\n"
              "Type=Link\n"
              "Name=A Link\n"
              "URL=https://example.com\n");

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  auto candidates = scanner.ListCandidates();
  REQUIRE(candidates.has_value());

  // Not an exact-size assertion: /var/lib/flatpak/exports is scanned
  // unconditionally (by design, for production reliability), so a real
  // machine's own installed Flatpak apps may legitimately add candidates
  // beyond this fixture's own two.
  bool found_flatpak = false, found_native = false;
  for (const auto& c : *candidates) {
    CHECK(c.id != "celeste");            // Mira's own entry, never a candidate
    CHECK(c.name != "Some Steam Game");  // Steam-style Exec=, covered by SteamScanner
    CHECK(c.name != "Hidden Thing");     // NoDisplay=true
    CHECK(c.name != "A Link");           // Type=Link, not Application
    if (c.id == "com.example.App") {
      found_flatpak = true;
      CHECK(c.name == "Example App");
      CHECK(c.icon == "com.example.App");
    }
    if (c.id == "native-game") {
      found_native = true;
      CHECK(c.name == "Native Game");
    }
  }
  CHECK(found_flatpak);
  CHECK(found_native);
}

TEST_CASE("DesktopEntryScanner: Import adds a Flatpak-style entry with the run-<app-id> convention") {
  Fixture fx("desktop-scan-import-flatpak");

  test::Touch(fx.apps_dir / "com.example.App.desktop",
              "[Desktop Entry]\n"
              "Type=Application\n"
              "Name=Example App\n"
              "Exec=flatpak run com.example.App\n"
              "X-Flatpak=com.example.App\n");

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  auto summary = scanner.Import({"com.example.App"});
  REQUIRE(summary.has_value());
  CHECK(summary->added == 1);
  REQUIRE(summary->added_games.size() == 1);

  const model::Game& game = summary->added_games[0];
  CHECK(game.name == "Example App");
  CHECK(game.exe_path == (fx.bin_dir / "flatpak").string());
  CHECK(game.args == "run com.example.App");
  CHECK(game.platform == model::Platform::Native);
  CHECK(game.install_path.ends_with(".var/app/com.example.App"));
  CHECK(game.status == model::GameStatus::Ready);
}

TEST_CASE("DesktopEntryScanner: Import adds a plain native entry, and re-importing updates rather than duplicates") {
  Fixture fx("desktop-scan-import-native");
  const fs::path exe_dir = TempDir("desktop-scan-import-native-exe");
  test::Touch(fx.apps_dir / "native-game.desktop", std::format("[Desktop Entry]\n"
                                                               "Type=Application\n"
                                                               "Name=Native Game\n"
                                                               "Exec=\"{}/game\" --fullscreen\n",
                                                               exe_dir.string()));

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  auto first = scanner.Import({"native-game"});
  REQUIRE(first.has_value());
  CHECK(first->added == 1);
  REQUIRE(first->added_games.size() == 1);
  CHECK(first->added_games[0].exe_path == (exe_dir / "game").string());
  CHECK(first->added_games[0].args == "--fullscreen");
  CHECK(first->added_games[0].install_path == exe_dir.string());

  auto second = scanner.Import({"native-game"});
  REQUIRE(second.has_value());
  CHECK(second->added == 0);
  CHECK(second->updated == 1);
  CHECK(fx.games.All().size() == 1);
}

TEST_CASE("DesktopEntryScanner: two bare-command entries import as two games and stay listed until imported") {
  Fixture fx("desktop-scan-import-bare");
  test::Touch(fx.apps_dir / "first.desktop",
              "[Desktop Entry]\nType=Application\nName=First\nExec=first-game\n");
  test::Touch(fx.apps_dir / "second.desktop",
              "[Desktop Entry]\nType=Application\nName=Second\nExec=second-game\n");

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  REQUIRE(scanner.Import({"first"}).has_value());

  auto candidates = scanner.ListCandidates();
  REQUIRE(candidates.has_value());
  const auto listed = [&](const char* id) {
    return std::ranges::contains(*candidates, std::string(id), &desktop::DesktopEntryCandidate::id);
  };
  CHECK_FALSE(listed("first"));
  CHECK(listed("second"));

  auto second = scanner.Import({"second"});
  REQUIRE(second.has_value());
  CHECK(second->added == 1);
  REQUIRE(fx.games.All().size() == 2);
  CHECK(fx.games.All()[0].exe_path == (fx.bin_dir / "first-game").string());
  CHECK(fx.games.All()[1].exe_path == (fx.bin_dir / "second-game").string());
}

TEST_CASE("DesktopEntryScanner: an entry whose command isn't installed isn't offered") {
  Fixture fx("desktop-scan-missing-command");
  test::Touch(fx.apps_dir / "gone.desktop",
              "[Desktop Entry]\nType=Application\nName=Gone\nExec=not-installed\n");

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  auto candidates = scanner.ListCandidates();
  REQUIRE(candidates.has_value());
  CHECK_FALSE(std::ranges::contains(*candidates, std::string("gone"), &desktop::DesktopEntryCandidate::id));
}

TEST_CASE("DesktopEntryScanner: re-importing an app imported with a bare command updates it to an absolute one") {
  Fixture fx("desktop-scan-import-legacy");
  test::Touch(
      fx.apps_dir / "com.example.App.desktop",
      "[Desktop Entry]\nType=Application\nName=Example App\nExec=flatpak run com.example.App\n"
      "X-Flatpak=com.example.App\n");
  model::Game legacy;
  legacy.id = "example-app";
  legacy.source = "desktop-entry";
  legacy.exe_path = "flatpak";
  legacy.args = "run com.example.App";
  REQUIRE(fx.games.Upsert(legacy).has_value());

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  auto summary = scanner.Import({"com.example.App"});
  REQUIRE(summary.has_value());
  CHECK(summary->updated == 1);
  REQUIRE(fx.games.All().size() == 1);
  CHECK(fx.games.All()[0].exe_path == (fx.bin_dir / "flatpak").string());
}

TEST_CASE("DesktopEntryScanner: an imported app gets no Mira menu entry by default, and keeps a user's choice") {
  Fixture fx("desktop-scan-import-menu");
  test::Touch(fx.apps_dir / "app.desktop",
              "[Desktop Entry]\nType=Application\nName=App\nExec=app-game\n");

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  auto added = scanner.Import({"app"});
  REQUIRE(added.has_value());
  REQUIRE(added->added_games.size() == 1);
  CHECK(added->added_games[0].overrides["desktop_entries.enabled"] == false);

  const std::string id = added->added_games[0].id;
  REQUIRE(fx.games.Update(id, [](model::Game& game) { game.overrides["desktop_entries.enabled"] = true; }));
  REQUIRE(scanner.Import({"app"}).has_value());
  CHECK(fx.games.Find(id)->overrides["desktop_entries.enabled"] == true);
}
