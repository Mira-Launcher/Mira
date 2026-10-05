#include <doctest.h>

#include "client/MiradClient.h"

using namespace mira_gui;

// The SSE payload parsers. These decide what a live event does to the
// library on screen, and they run on data the frontend never requested, so
// a malformed or unexpected payload has to be a no-op, never a crash and
// never a half-applied update.

TEST_CASE("ParseGameSummary accepts the full record game.added carries") {
  GameSummary game;
  REQUIRE(MiradClient::ParseGameSummary(
      R"({"id": "blue-prince", "name": "Blue Prince", "status": "ready",
          "platform": "windows", "confidence": 1.0, "play_seconds": 0})",
      &game));
  CHECK(game.id == "blue-prince");
  CHECK(game.name == "Blue Prince");
  CHECK(game.status == "ready");
}

TEST_CASE("ParseGameSummary rejects anything that isn't a JSON object") {
  GameSummary game;
  CHECK_FALSE(MiradClient::ParseGameSummary("", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary("not json", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary("[]", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary("null", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary("42", &game));
}

TEST_CASE("ParseGameSummary rejects a payload from a different event") {
  // The real case: runners.download.started shares the event stream with
  // game.added, and without an id check its payload parsed into a game with
  // every field empty, one blank tile in the library per runner download.
  GameSummary game;
  CHECK_FALSE(MiradClient::ParseGameSummary(R"({"kind": "proton", "tag": "GE-Proton11-7"})", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary(R"({"id": ""})", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary(R"({"id": 42})", &game));
}

TEST_CASE("ParseGameSummaries reads every game a batch changed, skipping entries without an id") {
  std::vector<GameSummary> games;
  REQUIRE(MiradClient::ParseGameSummaries(
      R"({"games": [{"id": "a", "tags": ["hidden"]}, {"name": "no id"}, {"id": "b", "tags": []}]})", &games));
  REQUIRE(games.size() == 2);
  CHECK(games[0].id == "a");
  CHECK(games[0].tags == std::vector<std::string>{"hidden"});
  CHECK(games[1].id == "b");

  CHECK_FALSE(MiradClient::ParseGameSummaries(R"({"id": "a"})", &games));
  CHECK_FALSE(MiradClient::ParseGameSummaries("not json", &games));
}

TEST_CASE("ParseArtThumbsEvent reads which previews are ready and which failed") {
  ArtThumbsEvent event;
  REQUIRE(MiradClient::ParseArtThumbsEvent(
      R"({"id": "hades", "type": "hero", "ready": [1, -1], "failed": [7, "x"]})", &event));
  CHECK(event.id == "hades");
  CHECK(event.slot == "hero");
  CHECK(event.ready == std::vector<std::int64_t>{1, -1});
  CHECK(event.failed == std::vector<std::int64_t>{7});
  CHECK(event.error.empty());

  CHECK_FALSE(MiradClient::ParseArtThumbsEvent(R"({"type": "hero"})", &event));
  CHECK_FALSE(MiradClient::ParseArtThumbsEvent("not json", &event));
}

TEST_CASE("ParseArtCandidatesEvent reads a page of candidates, or why there isn't one") {
  ArtCandidatesEvent event;
  REQUIRE(MiradClient::ParseArtCandidatesEvent(
      R"({"id": "ripples", "type": "cover", "page": 1, "total": 16, "request": "42",
          "candidates": [{"id": 9, "style": "alternate", "nsfw": true}]})",
      &event));
  CHECK(event.page == 1);
  CHECK(event.total == 16);
  CHECK(event.request == "42");
  REQUIRE(event.candidates.size() == 1);
  CHECK(event.candidates[0].nsfw);
  CHECK(event.candidates[0].source == "steamgriddb");

  REQUIRE(MiradClient::ParseArtCandidatesEvent(
      R"({"id": "ripples", "type": "cover", "page": 0, "code": "no_steamgriddb_key", "error": "set it"})", &event));
  CHECK(event.code == "no_steamgriddb_key");
  CHECK(event.candidates.empty());
}

TEST_CASE("ParseMetadataEvent carries the code, not just the message") {
  // The UI branches on the code. It exists precisely so that deciding what
  // to do about a failure never means pattern-matching English prose.
  MetadataEvent event;
  REQUIRE(MiradClient::ParseMetadataEvent(
      R"({"id": "blue-prince", "code": "no_steamgriddb_key", "error": "set steamgriddb.api_key"})",
      &event));
  CHECK(event.id == "blue-prince");
  CHECK(event.code == "no_steamgriddb_key");
  CHECK(event.error == "set steamgriddb.api_key");

  // game.metadata_ready carries an id and nothing else.
  REQUIRE(MiradClient::ParseMetadataEvent(R"({"id": "x"})", &event));
  CHECK(event.code.empty());
  CHECK(event.error.empty());

  CHECK_FALSE(MiradClient::ParseMetadataEvent(R"({"code": "x"})", &event));
  CHECK_FALSE(MiradClient::ParseMetadataEvent("not json", &event));
}

TEST_CASE("ParseGameState reads the launch/exit signal") {
  GameStateEvent state;
  REQUIRE(MiradClient::ParseGameState(R"({"id": "x", "state": "running", "pid": 1234})", &state));
  CHECK(state.id == "x");
  CHECK(state.state == "running");

  REQUIRE(MiradClient::ParseGameState(
      R"({"id": "x", "state": "crashed", "exit_code": 139, "played_seconds": 11})", &state));
  CHECK(state.state == "crashed");
}

TEST_CASE("ParseGameState requires an id") {
  // Without one there is no row to act on, and acting on the wrong row is
  // worse than ignoring the event.
  GameStateEvent state;
  CHECK_FALSE(MiradClient::ParseGameState(R"({"state": "running"})", &state));
  CHECK_FALSE(MiradClient::ParseGameState("not json", &state));
}

TEST_CASE("ParseRemovedIds reads games.removed and ignores anything malformed") {
  CHECK(MiradClient::ParseRemovedIds(R"({"ids": ["a", "b"]})") == std::vector<std::string>{"a", "b"});
  CHECK(MiradClient::ParseRemovedIds(R"({"ids": ["a", 3, ""]})") == std::vector<std::string>{"a"});
  CHECK(MiradClient::ParseRemovedIds(R"({"id": "a"})").empty());
  CHECK(MiradClient::ParseRemovedIds("not json").empty());
}

TEST_CASE("ParseRemovedId returns an empty id rather than throwing") {
  CHECK(MiradClient::ParseRemovedId(R"({"id": "wandering-sword"})") == "wandering-sword");
  CHECK(MiradClient::ParseRemovedId(R"({})").empty());
  CHECK(MiradClient::ParseRemovedId("[]").empty());
  CHECK(MiradClient::ParseRemovedId("").empty());
}

TEST_CASE("ParseRunnerDownload reads the state from the event type and ignores unrelated events") {
  // The payload doesn't repeat which of started/finished/failed it is, that
  // only exists in the SSE `event:` line. HandleEvent sees every event on the
  // stream, so the type check is what keeps a game.updated from being read
  // as a download.
  RunnerDownloadEvent event;
  REQUIRE(MiradClient::ParseRunnerDownload(
      "runners.download.finished", R"({"kind": "proton", "tag": "GE-Proton11-7"})", &event));
  CHECK(event.state == "finished");
  CHECK(event.kind == "proton");
  CHECK(event.tag == "GE-Proton11-7");
  CHECK(event.error.empty());

  REQUIRE(MiradClient::ParseRunnerDownload(
      "runners.download.failed", R"({"kind": "wine", "tag": "x", "error": "checksum mismatch"})",
      &event));
  CHECK(event.state == "failed");
  CHECK(event.error == "checksum mismatch");

  CHECK(MiradClient::ParseRunnerDownload("runners.download.started", R"({})", &event));
  CHECK_FALSE(MiradClient::ParseRunnerDownload("game.updated", R"({"id": "x"})", &event));
  CHECK_FALSE(MiradClient::ParseRunnerDownload("runners.updated", R"({})", &event));
  CHECK_FALSE(MiradClient::ParseRunnerDownload("runners.download.finished", "not json", &event));
}

TEST_CASE("ParseTricksEvent ignores unrelated event types") {
  // Same shared-stream hazard as runner downloads: a WinetricksDialog must
  // not mistake a game.updated (or someone else's tricks event) for its own.
  TricksEvent event;
  CHECK_FALSE(MiradClient::ParseTricksEvent("game.added", R"({"id": "x"})", &event));
  CHECK_FALSE(MiradClient::ParseTricksEvent("runners.download.started", R"({})", &event));
  CHECK(MiradClient::ParseTricksEvent("tricks.started", R"({})", &event));
}

TEST_CASE("ParseTricksEvent extracts id, verb, state and error") {
  TricksEvent event;
  REQUIRE(MiradClient::ParseTricksEvent("tricks.failed",
                                        R"({"id": "x", "verb": "corefonts", "error": "winetricks not found"})",
                                        &event));
  CHECK(event.id == "x");
  CHECK(event.verb == "corefonts");
  CHECK(event.state == "failed");
  CHECK(event.error == "winetricks not found");

  REQUIRE(MiradClient::ParseTricksEvent("tricks.finished", R"({"id": "x", "verb": "corefonts"})",
                                        &event));
  CHECK(event.state == "finished");
  CHECK(event.error.empty());
}
