#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

#include "config/Config.h"
#include "desktop/DesktopEntries.h"
#include "store/MetadataStore.h"
#include "metadata/MetadataFetcher.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

TEST_CASE("DesktopEntries never writes an entry for a Steam-sourced game -- Steam already has one") {
  const fs::path state = TempDir("desktop-entries-steam-state");
  const fs::path applications = TempDir("desktop-entries-steam-apps");

  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("desktop_entries.enabled", true).has_value());
  // Point it at a scratch dir rather than the real
  // ~/.local/share/applications.
  REQUIRE(config.Set("desktop_entries.directory", applications.string()).has_value());

  model::Game steam_game;
  steam_game.id = "some-steam-game";
  steam_game.name = "Some Steam Game";
  steam_game.status = model::GameStatus::Ready;
  steam_game.runner_ref = "steam:504230";
  // Deliberately no exe_path -- a Steam game in "steam" launch mode never
  // has one, and used to still get an entry regardless (see IsLaunchable).

  model::Game native_game;
  native_game.id = "native-game";
  native_game.name = "Native Game";
  native_game.status = model::GameStatus::Ready;
  native_game.exe_path = "game";

  store::MetadataStore cache(config.File().parent_path());
  cache.Load();
  desktop::DesktopEntries entries(config, cache);
  REQUIRE(entries.Sync({steam_game, native_game}).has_value());

  CHECK_FALSE(fs::exists(applications / "mira-some-steam-game.desktop"));
  CHECK(fs::exists(applications / "mira-native-game.desktop"));
}

TEST_CASE("DesktopEntries: an app-tagged entry is filed under Utility, and a Microsoft 365 app opens its files") {
  const fs::path state = TempDir("desktop-entries-app-state");
  const fs::path applications = TempDir("desktop-entries-app-apps");
  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("desktop_entries.enabled", true).has_value());
  REQUIRE(config.Set("desktop_entries.directory", applications.string()).has_value());

  model::Game app;
  app.id = "writer";
  app.name = "Writer";
  app.status = model::GameStatus::Ready;
  app.exe_path = "writer.exe";
  app.tags = {"app"};

  model::Game word = app;
  word.id = "office-word";
  word.source = "office";
  word.source_ref = "word";

  store::MetadataStore cache(config.File().parent_path());
  cache.Load();
  desktop::DesktopEntries entries(config, cache);
  REQUIRE(entries.Sync({app, word}).has_value());

  const auto read = [&](const char* file) {
    std::ifstream in(applications / file);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const std::string text = read("mira-writer.desktop");
  CHECK(text.find("Categories=Utility;") != std::string::npos);
  CHECK(text.find("Icon=application-x-executable") != std::string::npos);
  CHECK(text.find("MimeType=") == std::string::npos);
  const std::string word_text = read("mira-office-word.desktop");
  CHECK(word_text.find("mira launch office-word %F") != std::string::npos);
  CHECK(word_text.find("StartupWMClass=steam_app_mira_officeword\n") != std::string::npos);
  CHECK(word_text.find("MimeType=application/vnd.openxmlformats-officedocument.wordprocessingml.document;") !=
        std::string::npos);
}

TEST_CASE("DesktopEntries: a per-game desktop_entries.enabled=false override excludes just that game") {
  const fs::path state = TempDir("desktop-entries-override-state");
  const fs::path applications = TempDir("desktop-entries-override-apps");

  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("desktop_entries.enabled", true).has_value());
  REQUIRE(config.Set("desktop_entries.directory", applications.string()).has_value());

  model::Game excluded;
  excluded.id = "excluded-game";
  excluded.name = "Excluded Game";
  excluded.status = model::GameStatus::Ready;
  excluded.exe_path = "game";
  excluded.overrides = {{"desktop_entries.enabled", false}};

  model::Game included;
  included.id = "included-game";
  included.name = "Included Game";
  included.status = model::GameStatus::Ready;
  included.exe_path = "game";

  store::MetadataStore cache(config.File().parent_path());
  cache.Load();
  desktop::DesktopEntries entries(config, cache);
  REQUIRE(entries.Sync({excluded, included}).has_value());

  CHECK_FALSE(fs::exists(applications / "mira-excluded-game.desktop"));
  CHECK(fs::exists(applications / "mira-included-game.desktop"));
}

TEST_CASE("DesktopEntries: a sync leaves unchanged entries untouched but rewrites a changed one") {
  const fs::path state = TempDir("desktop-entries-unchanged-state");
  const fs::path applications = TempDir("desktop-entries-unchanged-apps");

  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("desktop_entries.enabled", true).has_value());
  REQUIRE(config.Set("desktop_entries.directory", applications.string()).has_value());

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  game.status = model::GameStatus::Ready;
  game.exe_path = "Celeste";

  store::MetadataStore cache(config.File().parent_path());
  cache.Load();
  desktop::DesktopEntries entries(config, cache);
  REQUIRE(entries.Sync({game}).has_value());
  const fs::path file = applications / "mira-celeste.desktop";
  const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(1);
  fs::last_write_time(file, old_time);

  REQUIRE(entries.Sync({game}).has_value());
  CHECK(fs::last_write_time(file) == old_time);

  game.name = "Celeste Classic";
  REQUIRE(entries.Sync({game}).has_value());
  std::ifstream in(file);
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.find("Name=Celeste Classic\n") != std::string::npos);
}

TEST_CASE("DesktopEntries: uses cached artwork as Icon= when present, falls back otherwise") {
  const fs::path state = TempDir("desktop-entries-icon-state");
  const fs::path applications = TempDir("desktop-entries-icon-apps");

  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("desktop_entries.enabled", true).has_value());
  REQUIRE(config.Set("desktop_entries.directory", applications.string()).has_value());

  const fs::path artwork_dir = metadata::ArtworkDir(config, "with-art");
  fs::create_directories(artwork_dir);
  std::ofstream(artwork_dir / "cover.png") << "not really a png";

  model::Game with_art;
  with_art.id = "with-art";
  with_art.name = "With Art";
  with_art.status = model::GameStatus::Ready;
  with_art.exe_path = "game";

  model::Game without_art;
  without_art.id = "without-art";
  without_art.name = "Without Art";
  without_art.status = model::GameStatus::Ready;
  without_art.exe_path = "game";

  store::MetadataStore cache(config.File().parent_path());
  cache.Load();
  REQUIRE(cache.Write("with-art", nlohmann::json::parse(R"({"artwork": {"file": "cover.png"}})")).has_value());
  desktop::DesktopEntries entries(config, cache);
  REQUIRE(entries.Sync({with_art, without_art}).has_value());

  std::ifstream with_art_in(applications / "mira-with-art.desktop");
  std::string with_art_contents((std::istreambuf_iterator<char>(with_art_in)), std::istreambuf_iterator<char>());
  CHECK(with_art_contents.find(std::format("Icon={}", (artwork_dir / "cover.png").string())) != std::string::npos);

  std::ifstream without_art_in(applications / "mira-without-art.desktop");
  std::string without_art_contents((std::istreambuf_iterator<char>(without_art_in)),
                                   std::istreambuf_iterator<char>());
  CHECK(without_art_contents.find("Icon=applications-games") != std::string::npos);
}
