#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <json.hpp>

#include "api/EventBus.h"
#include "config/Config.h"
#include "metadata/FetchQueue.h"
#include "metadata/MetadataFetcher.h"
#include "model/Types.h"
#include "store/MetadataStore.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

// Everything here deliberately exercises only the non-Steam path with no
// steamgriddb.api_key set: that's the one branch that's fully offline (see
// MetadataFetcher.cpp's FetchNonSteam: metadata.protondb_for_non_steam
// defaults to false for the same reason, so it adds no network call here
// either), so these stay hermetic without mocking curl. The
// Steam/ProtonDB/SteamGridDB paths themselves were verified live against
// real APIs during development, not here.

TEST_CASE("Fetch on a non-Steam game with no SteamGridDB key fails, and caches nothing") {
  // SteamGridDB is the only free cover source for a non-Steam game, so with
  // no key there is nothing this could have tried. Reporting success left a
  // cache file and a game.metadata_ready event behind, which reads as
  // "looked and found nothing", and the next attempt then looked answered.
  const fs::path dir = TempDir("metadata-nonsteam");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();
  test::Isolate(config);

  model::Game game;
  game.id = "some-game";
  game.name = "Some Game";
  // runner_ref left empty -> not Steam-owned.

  const Result<void> fetched = metadata::Fetch(config, cache, game);
  REQUIRE_FALSE(fetched.has_value());
  CHECK(fetched.error().code == "no_steamgriddb_key");
  // Points clients at the setting, because it is the whole remedy.
  CHECK(fetched.error().fix.kind == "setting");
  CHECK(fetched.error().fix.target == "steamgriddb.api_key");

  CHECK_FALSE(cache.Has(game.id));
  CHECK_FALSE(fs::exists(metadata::ArtworkDir(config, game.id)));
}

TEST_CASE("Metadata/artwork cache paths sit next to settings.toml, not a global XDG lookup") {
  const fs::path dir = TempDir("metadata-paths");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();

  CHECK(metadata::ArtworkDir(config, "foo") == dir / "artwork" / "foo");
  CHECK(cache.ArtworkDir("foo") == metadata::ArtworkDir(config, "foo"));
}

TEST_CASE("metadata/*.json files are imported into the cache once, with their art") {
  const fs::path dir = TempDir("metadata-import");
  fs::create_directories(dir / "metadata");
  fs::create_directories(dir / "artwork" / "celeste");
  std::ofstream(dir / "artwork" / "celeste" / "cover.png") << "png";
  std::ofstream(dir / "metadata" / "celeste.json") << R"({"artwork": {"file": "cover.png", "content_type": "image/png"}})";
  std::ofstream(dir / "metadata" / "broken.json") << "{ not json";

  store::MetadataStore cache(dir);
  cache.Load();
  CHECK(cache.Has("celeste"));
  CHECK_FALSE(cache.Has("broken"));
  REQUIRE(cache.ArtFor("celeste", "cover").has_value());
  CHECK(cache.ArtFor("celeste", "cover")->content_type == "image/png");
  CHECK(cache.ArtVersions("celeste").contains("cover"));
  CHECK_FALSE(fs::exists(dir / "metadata"));

  cache.Remove("celeste");
  CHECK_FALSE(cache.Has("celeste"));
  CHECK_FALSE(fs::exists(dir / "artwork" / "celeste"));
}

TEST_CASE("SelectArtwork fails closed: no metadata, no candidate list, unknown id") {
  const fs::path dir = TempDir("metadata-select-artwork");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();

  // Nothing fetched yet for this game at all.
  CHECK_FALSE(metadata::SelectArtwork(config, cache, "no-such-game", "hero", 1).has_value());

  {
    // Has a metadata file, but no art_candidates for "hero" -- e.g. a
    // Steam-owned game, which never populates that key at all.
    REQUIRE(cache.Write("celeste", nlohmann::json{{"source", "steam"}}).has_value());
  }
  CHECK_FALSE(metadata::SelectArtwork(config, cache, "celeste", "hero", 1).has_value());

  {
    REQUIRE(cache.Write("celeste", nlohmann::json{
        {"art_candidates", {{"hero", nlohmann::json::array({{{"id", 42}, {"url", "https://example.invalid/a.jpg"}}})}}},
    }).has_value());
  }
  // Right slot, wrong id.
  CHECK_FALSE(metadata::SelectArtwork(config, cache, "celeste", "hero", 999).has_value());
}

TEST_CASE("FetchCandidatePage fails before asking SteamGridDB when it can't") {
  const fs::path dir = TempDir("metadata-candidate-page");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();

  CHECK(metadata::FetchCandidatePage(config, cache, "celeste", "banner", 0).error().code == "invalid_type");
  CHECK(metadata::FetchCandidatePage(config, cache, "celeste", "cover", 0).error().code == "no_steamgriddb_key");

  // A key, but no SteamGridDB match recorded for the game.
  REQUIRE(config.Set("steamgriddb.api_key", std::string("test-key")).has_value());
  CHECK(metadata::FetchCandidatePage(config, cache, "celeste", "cover", 0).error().code == "no_steamgriddb_match");
}

TEST_CASE("SelectArtwork records the pick and points the slot at its file") {
  const fs::path dir = TempDir("metadata-select-atomic");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();

  const fs::path image = dir / "new.png";
  std::ofstream(image, std::ios::binary) << "\x89PNG new";
  REQUIRE(cache.Write("celeste", nlohmann::json{
      {"art_candidates", {{"cover", nlohmann::json::array({{{"id", 5}, {"url", "file://" + image.string()}}})}}},
  }).has_value());

  REQUIRE(metadata::SelectArtwork(config, cache, "celeste", "cover", 5).has_value());
  const nlohmann::json info = cache.Read("celeste");
  CHECK(info["artwork"].value("candidate_id", 0) == 5);
  CHECK(cache.ArtFor("celeste", "cover").has_value());
}

TEST_CASE("A refresh keeps art the user picked by hand") {
  const fs::path dir = TempDir("metadata-refresh-keeps-chosen");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();
  REQUIRE(config.Set("metadata.steam_art_by_name", false).has_value());  // keeps Fetch offline

  const fs::path image = dir / "picked.png";
  std::ofstream(image, std::ios::binary) << "\x89PNG picked";
  REQUIRE(cache.Write("celeste", nlohmann::json{
      {"art_candidates", {{"cover", nlohmann::json::array({{{"id", 5}, {"url", "file://" + image.string()}}})}}},
  }).has_value());
  REQUIRE(metadata::SelectArtwork(config, cache, "celeste", "cover", 5).has_value());

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  REQUIRE(metadata::Fetch(config, cache, game).has_value());

  const nlohmann::json info = cache.Read("celeste");
  CHECK(info["artwork"].value("candidate_id", 0) == 5);
  std::ifstream cover(metadata::ArtworkDir(config, "celeste") / info["artwork"].value("file", std::string()),
                      std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(cover)), std::istreambuf_iterator<char>());
  CHECK(bytes == "\x89PNG picked");
}

TEST_CASE("SelectArtwork that fails to download keeps the slot's current image") {
  const fs::path dir = TempDir("metadata-select-keeps");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();

  const fs::path art = metadata::ArtworkDir(config, "celeste");
  fs::create_directories(art);
  std::ofstream(art / "cover.png") << "current";
  REQUIRE(cache.Write("celeste", nlohmann::json{
      {"artwork", {{"file", "cover.png"}, {"content_type", "image/png"}}},
      {"art_candidates",
       {{"cover", nlohmann::json::array({{{"id", 5}, {"url", "file://" + (dir / "missing.png").string()}}})}}},
  }).has_value());

  CHECK_FALSE(metadata::SelectArtwork(config, cache, "celeste", "cover", 5).has_value());
  std::ifstream in(art / "cover.png");
  std::string kept;
  in >> kept;
  CHECK(kept == "current");
}

TEST_CASE("FetchCandidateThumbs caches each listed preview and reports which failed") {
  // file:// candidates keep this offline: curl fetches them the same way.
  const fs::path dir = TempDir("metadata-thumbs");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();

  const fs::path image = dir / "thumb.png";
  std::ofstream(image, std::ios::binary) << "\x89PNG fake";
  REQUIRE(cache.Write("celeste", nlohmann::json{
      {"art_candidates",
       {{"cover", nlohmann::json::array({
                      {{"id", 1}, {"url", "file://" + image.string()}},
                      {{"id", 2}, {"url", "https://example.invalid/big.jpg"}, {"thumb", "file://" + image.string()}},
                      {{"id", 3}, {"url", "file://" + (dir / "missing.png").string()}},
                  })}}},
  }).has_value());

  CHECK(metadata::CandidateThumbFile(config, "celeste", "cover", 1).empty());
  const Result<metadata::ThumbBatch> batch = metadata::FetchCandidateThumbs(config, cache, "celeste", "cover", {1, 2, 3, 4});
  REQUIRE(batch.has_value());
  CHECK(std::set<std::int64_t>(batch->ready.begin(), batch->ready.end()) == std::set<std::int64_t>{1, 2});
  // 3's file doesn't exist; 4 isn't a candidate at all.
  CHECK(std::set<std::int64_t>(batch->failed.begin(), batch->failed.end()) == std::set<std::int64_t>{3, 4});
  CHECK_FALSE(metadata::CandidateThumbFile(config, "celeste", "cover", 2).empty());
  CHECK(metadata::CandidateThumbFile(config, "celeste", "cover", 3).empty());

  // Cached now: a second batch doesn't fetch it again.
  fs::remove(image);
  const Result<metadata::ThumbBatch> again = metadata::FetchCandidateThumbs(config, cache, "celeste", "cover", {1});
  REQUIRE(again.has_value());
  CHECK(again->ready == std::vector<std::int64_t>{1});

  // The slot names a file, so anything but a plain word is refused.
  CHECK_FALSE(metadata::FetchCandidateThumbs(config, cache, "celeste", "../cover", {1}).has_value());
  CHECK(metadata::CandidateThumbFile(config, "celeste", "../cover", 1).empty());

  // Kept apart from the game's art, which a clear leaves alone.
  CHECK_FALSE(fs::exists(metadata::ArtworkDir(config, "celeste") / "thumbs"));
  metadata::ClearCandidateThumbs(config);
  CHECK(metadata::CandidateThumbFile(config, "celeste", "cover", 1).empty());
}

TEST_CASE("Enqueue skips when metadata is off unless forced; a failure is only metadata_failed") {
  // With no key every fetch fails offline, so the published events show which
  // fetches ran. metadata_ready has to mean there is something to show, or a
  // frontend refreshing its art on it refreshes into the same placeholder.
  const fs::path dir = TempDir("metadata-queue");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();
  test::Isolate(config);
  REQUIRE(config.Set("metadata.enabled", false).has_value());

  const auto run = [&config, &cache](bool force) {
    api::EventBus events;
    model::Game game;
    game.id = "celeste";
    game.name = "Celeste";
    {
      metadata::FetchQueue queue(cache);
      queue.Enqueue(config, events, game, force);
      queue.WaitIdle();
    }
    return events.Since(0);
  };

  CHECK(run(/*force=*/false).empty());

  const std::vector<model::Event> forced = run(/*force=*/true);
  REQUIRE(forced.size() == 1);
  CHECK(forced[0].type == "game.metadata_failed");
  CHECK(forced[0].payload.value("id", std::string()) == "celeste");
  CHECK(forced[0].payload.value("code", std::string()) == "no_steamgriddb_key");

  REQUIRE(config.Set("metadata.enabled", true).has_value());
  const std::vector<model::Event> enabled = run(/*force=*/false);
  REQUIRE(enabled.size() == 1);
  CHECK(enabled[0].type == "game.metadata_failed");
}

TEST_CASE("FetchQueue runs every queued game once, through a bounded set of workers") {
  const fs::path dir = TempDir("metadata-bulk");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::MetadataStore cache(dir);
  cache.Load();
  // No key and no Steam lookup: each fetch fails fast, offline.
  REQUIRE(config.Set("metadata.steam_art_by_name", false).has_value());

  api::EventBus events;
  {
    metadata::FetchQueue queue(cache);
    for (int i = 0; i < 40; ++i) {
      model::Game game;
      game.id = "bulk-" + std::to_string(i);
      game.name = game.id;
      queue.Enqueue(config, events, game, /*force=*/true);
    }
    queue.WaitIdle();
  }

  std::set<std::string> fetched;
  for (const model::Event& event : events.Since(0)) {
    if (event.type == "game.metadata_failed") fetched.insert(event.payload.value("id", std::string()));
  }
  CHECK(fetched.size() == 40);
}
