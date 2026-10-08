#include <doctest.h>

#include <fstream>
#include <filesystem>

#include "config/Config.h"
#include "config/Resolver.h"
#include "config/Schema.h"

using namespace mira::config;
namespace fs = std::filesystem;

TEST_CASE("every schema entry is well-formed") {
  // Guards the registry itself: a key with no label or an invalid default
  // would silently produce a broken settings UI, so this is checked
  // mechanically rather than by review. A doc may be empty when the label
  // says it all.
  for (const Entry& entry : Schema::Instance().Entries()) {
    INFO("key: ", entry.key);
    CHECK_FALSE(entry.label.empty());
    CHECK_FALSE(entry.category.empty());
    CHECK_FALSE(entry.group_label.empty());
    CHECK_FALSE(Schema::Instance().Validate(entry.key, entry.default_value).has_value());
  }
}

TEST_CASE("settings are attributed to the source they configure") {
  const Schema& schema = Schema::Instance();
  CHECK(schema.Find("steam.root")->source == "steam");
  CHECK(schema.Find("gog.install_root")->source == "gog");
  CHECK(schema.Find("launchers.ubisoft.disable_overlay")->source == "ubisoft");
  // A download location for a store's tool and a launcher-wide setting belong to no one source.
  CHECK(schema.Find("runner_sources.gog.repo")->source.empty());
  CHECK(schema.Find("launchers.runner")->source.empty());
  CHECK(schema.Find("scan.max_depth")->source.empty());
}

TEST_CASE("every setting on the Sources page belongs to a source") {
  for (const Entry& entry : Schema::Instance().Entries()) {
    if (entry.category != "Sources") continue;
    INFO("key: ", entry.key);
    CHECK_FALSE(entry.source.empty());
  }
}

TEST_CASE("Schema::Validate rejects bad values with a reason") {
  const Schema& schema = Schema::Instance();
  CHECK_FALSE(schema.Validate("scan.debounce_ms", 5000).has_value());
  CHECK(schema.Validate("scan.debounce_ms", -1).has_value());
  CHECK(schema.Validate("scan.debounce_ms", "not a number").has_value());
  CHECK(schema.Validate("prefix_naming", "not_a_real_option").has_value());
  CHECK(schema.Validate("nonexistent.key", 1).has_value());
}

namespace {
fs::path TempFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests";
  fs::create_directories(dir);
  return dir / name;
}

std::int64_t DefaultDebounce() {
  return Schema::Instance().Find("scan.debounce_ms")->default_value.get<std::int64_t>();
}
}  // namespace

TEST_CASE("Config materialises defaults when no file exists") {
  const fs::path file = TempFile("settings-defaults.toml");
  fs::remove(file);
  Config config(file);
  config.Load();
  CHECK(fs::exists(file));
  CHECK(config.GetInt("scan.debounce_ms") == DefaultDebounce());
}

TEST_CASE("Config round-trips a value and preserves frontend settings") {
  const fs::path file = TempFile("settings-roundtrip.toml");
  fs::remove(file);
  Config config(file);
  config.Load();
  CHECK(config.Set("scan.debounce_ms", 9000).has_value());
  config.SetFrontendSettings({{"theme", "dark"}});

  Config reloaded(file);
  reloaded.Load();
  CHECK(reloaded.GetInt("scan.debounce_ms") == 9000);
  CHECK(reloaded.FrontendSettings().value("theme", "") == "dark");
}

TEST_CASE("frontend settings live in their own file, never leaking into settings.toml") {
  const fs::path file = TempFile("settings-frontend-split.toml");
  fs::remove(file);
  const fs::path frontend_file = file.parent_path() / "frontend.toml";
  fs::remove(frontend_file);

  Config config(file);
  config.Load();
  REQUIRE(config.Patch({{"frontend", {{"theme", "dark"}}}, {"scan", {{"debounce_ms", 9000}}}}).has_value());

  CHECK(fs::exists(frontend_file));
  std::ifstream backend(file);
  std::string backend_text((std::istreambuf_iterator<char>(backend)), std::istreambuf_iterator<char>());
  CHECK(backend_text.find("frontend") == std::string::npos);

  Config reloaded(file);
  reloaded.Load();
  CHECK(reloaded.GetInt("scan.debounce_ms") == 9000);
  CHECK(reloaded.FrontendSettings().value("theme", "") == "dark");
}

TEST_CASE("Config::Set rejects an invalid value and changes nothing") {
  const fs::path file = TempFile("settings-invalid.toml");
  fs::remove(file);
  Config config(file);
  config.Load();
  const auto before = config.GetInt("scan.debounce_ms");
  auto result = config.Set("scan.debounce_ms", -5);
  CHECK_FALSE(result.has_value());
  CHECK(config.GetInt("scan.debounce_ms") == before);
}

TEST_CASE("an unparseable settings file is quarantined, not fatal") {
  const fs::path file = TempFile("settings-corrupt.toml");
  {
    std::ofstream out(file);
    out << "this is not [ valid toml";
  }
  Config config(file);
  config.Load();  // must not throw or crash
  CHECK(config.GetInt("scan.debounce_ms") == DefaultDebounce());  // falls back to defaults
  CHECK(fs::exists(file.string() + ".bad"));
  fs::remove(file.string() + ".bad");

  // With the last settings that loaded, those come back instead of defaults.
  {
    std::ofstream out(file);
    out << "this is not [ valid toml";
  }
  const std::string last_good = "[scan]\ndebounce_ms = 1234\n";
  config.Load(last_good);
  CHECK(config.GetInt("scan.debounce_ms") == 1234);
  CHECK(config.LoadedText() == last_good);
  fs::remove(file.string() + ".bad");
}

TEST_CASE("Resolver layers game overrides above the config file above defaults") {
  const fs::path file = TempFile("settings-resolver.toml");
  fs::remove(file);
  Config config(file);
  config.Load();
  REQUIRE(config.Set("launch.log_max_mb", 6).has_value());

  Resolver no_override(config, nlohmann::json::object());
  CHECK(no_override.GetInt("launch.log_max_mb") == 6);
  CHECK(no_override.Resolve("launch.log_max_mb").layer == Layer::ConfigFile);

  Resolver with_override(config, nlohmann::json{{"launch.log_max_mb", 10}});
  CHECK(with_override.GetInt("launch.log_max_mb") == 10);
  CHECK(with_override.Resolve("launch.log_max_mb").layer == Layer::Game);

  // Daemon-only keys must not be overridable per game, even if a game
  // document somehow carries one.
  Resolver bogus(config, nlohmann::json{{"library_roots", nlohmann::json::array({"/tmp"})}});
  CHECK_FALSE(Resolver::IsOverridable("library_roots"));
  CHECK_FALSE(Resolver::IsOverridable("scan.max_depth"));
  CHECK(bogus.Resolve("library_roots").layer != Layer::Game);
}

TEST_CASE("a hand edit is picked up, survives an app change, and a broken one changes nothing") {
  const fs::path file = TempFile("settings-hand-edit.toml");
  Config config(file);
  config.Load();
  const auto write = [&file](const std::string& text) { std::ofstream(file, std::ios::trunc) << text; };

  write("[scan]\ndebounce_ms = 1234\n");
  auto changed = config.Reload();
  REQUIRE(changed.has_value());
  CHECK(*changed == std::vector<std::string>{"scan.debounce_ms"});
  CHECK(config.GetInt("scan.debounce_ms") == 1234);

  // An edit the watcher hasn't reported yet is merged, not saved over.
  write("[scan]\ndebounce_ms = 4321\n");
  REQUIRE(config.Set("log.level", std::string("debug")).has_value());
  CHECK(config.GetInt("scan.debounce_ms") == 4321);
  Config reread(file);
  reread.Load();
  CHECK(reread.GetInt("scan.debounce_ms") == 4321);
  CHECK(reread.GetString("log.level") == "debug");

  write("[scan\ndebounce_ms = 1\n");
  auto broken = config.Reload();
  REQUIRE_FALSE(broken.has_value());
  CHECK(broken.error().message.find("line 1") != std::string::npos);
  CHECK(config.GetInt("scan.debounce_ms") == 4321);
}
