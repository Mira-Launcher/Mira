#include <doctest.h>

#include <json.hpp>

#include "client/JsonMapping.h"
#include "client/MiradClient.h"

using nlohmann::json;
using namespace mira_gui;

// These cover what the UI believes each field means. A mismatch here shows
// up as a blank column rather than an error, so the interesting cases are
// the absent and wrongly-typed ones, not the happy path.

TEST_CASE("ToGameSummary reads the fields a library row shows") {
  const json entry = json::parse(R"({
    "id": "animal-well", "name": "Animal Well", "status": "ready",
    "platform": "windows", "runner_ref": "proton_umu:GE-Proton11-7",
    "last_error": "", "needs_check": true,
    "last_played_at": 1789620825, "play_seconds": 4210, "running": true
  })");

  const GameSummary game = mapping::ToGameSummary(entry);
  CHECK(game.running);
  CHECK(game.id == "animal-well");
  CHECK(game.name == "Animal Well");
  CHECK(game.status == "ready");
  CHECK(game.platform == "windows");
  CHECK(game.runner_ref == "proton_umu:GE-Proton11-7");
  CHECK(game.needs_check);
  REQUIRE(game.last_played_at.has_value());
  CHECK(*game.last_played_at == 1789620825);
  CHECK(game.play_seconds == 4210);
}

TEST_CASE("A record's art tells a game with no art apart from one that didn't say") {
  const GameSummary with = mapping::ToGameSummary(json::parse(R"({"id": "x", "art": {"cover": "a1-2", "hero": 3}})"));
  REQUIRE(with.art.has_value());
  CHECK(with.art->at("cover") == "a1-2");
  CHECK_FALSE(with.art->contains("hero"));  // not a version

  const GameSummary none = mapping::ToGameSummary(json::parse(R"({"id": "x", "art": {}})"));
  REQUIRE(none.art.has_value());
  CHECK(none.art->empty());

  CHECK_FALSE(mapping::ToGameSummary(json::parse(R"({"id": "x"})")).art.has_value());

  MetadataEvent event;
  REQUIRE(MiradClient::ParseMetadataEvent(R"({"id": "x", "art": {"cover": "b"}})", &event));
  REQUIRE(event.art.has_value());
  CHECK(event.art->at("cover") == "b");
}

TEST_CASE("ToGameSummary tolerates a record missing every optional field") {
  // mirad omits nothing today, but a summary is also built from an SSE
  // payload, and a trimmed event must not produce garbage: every absent
  // field falls back rather than throwing.
  const GameSummary game = mapping::ToGameSummary(json::parse(R"({"id": "x"})"));
  CHECK(game.id == "x");
  CHECK(game.name.empty());
  CHECK(game.status.empty());
  CHECK_FALSE(game.needs_check);
  CHECK(game.play_seconds == 0);
  CHECK_FALSE(game.last_played_at.has_value());
}

TEST_CASE("ToGameSummary treats a null last_played_at as never played") {
  // mirad sends null (not a missing key, not 0) for never played, and
  // "Never" has to survive that. A 0 is a real timestamp (1970), so these
  // must not collapse into each other.
  const GameSummary never =
      mapping::ToGameSummary(json::parse(R"({"id": "x", "last_played_at": null})"));
  CHECK_FALSE(never.last_played_at.has_value());

  const GameSummary epoch =
      mapping::ToGameSummary(json::parse(R"({"id": "x", "last_played_at": 0})"));
  REQUIRE(epoch.last_played_at.has_value());
  CHECK(*epoch.last_played_at == 0);
}

TEST_CASE("ToGameDetail keeps runner_config and env as JSON text") {
  // They are arbitrary objects with no fixed shape to build widgets for, so
  // the dialog round-trips them as text, which means an absent one has to
  // become "{}" and not an empty string, or saving an untouched game would
  // send an unparsable body.
  const GameDetail detail = mapping::ToGameDetail(json::parse(R"({"id": "x"})"));
  CHECK(detail.runner_config_json == "{}");
  CHECK(detail.env_json == "{}");

  const GameDetail with_env = mapping::ToGameDetail(json::parse(R"({
    "id": "x", "env": {"DXVK_HUD": "fps"}
  })"));
  CHECK(json::parse(with_env.env_json)["DXVK_HUD"] == "fps");
}

TEST_CASE("ToGameDetail reads every candidate, including installer flags") {
  const GameDetail detail = mapping::ToGameDetail(json::parse(R"({
    "id": "x",
    "candidates": [
      {"rel_path": "setup.exe", "kind": "windows", "score": 1.5,
       "chosen": false, "is_installer": true},
      {"rel_path": "game.exe", "kind": "windows", "score": 4.0, "chosen": true}
    ]
  })"));

  REQUIRE(detail.candidates.size() == 2);
  CHECK(detail.candidates[0].rel_path == "setup.exe");
  CHECK(detail.candidates[0].is_installer);
  CHECK_FALSE(detail.candidates[0].chosen);
  CHECK(detail.candidates[1].chosen);
  CHECK_FALSE(detail.candidates[1].is_installer);  // absent means false, not unset
  CHECK(detail.candidates[1].score == doctest::Approx(4.0));
}

TEST_CASE("ToDisplayString renders each scalar schema type as one editable line") {
  CHECK(mapping::ToDisplayString(json(true)) == "true");
  CHECK(mapping::ToDisplayString(json(false)) == "false");
  CHECK(mapping::ToDisplayString(json(42)) == "42");
  CHECK(mapping::ToDisplayString(json("~/Games")) == "~/Games");
}

TEST_CASE("A list setting round-trips unchanged, items with commas included") {
  const json original = json::parse(R"(["~/Games", "*.{txt,log}", "/mnt/a, b"])");
  CHECK(json(mapping::ParseListText(mapping::ToDisplayString(original))) == original);
  CHECK(mapping::TypedValueFromText("an array of strings", mapping::ListText({"*.{txt,log}"})) ==
        json::parse(R"(["*.{txt,log}"])"));
  CHECK(mapping::ParseListText(mapping::ToDisplayString(json::array())).empty());
}

TEST_CASE("A merge patch between two objects sends changed keys and removes cleared ones") {
  const json before = json::parse(R"({"gameid": "umu-123", "store": "ea", "kept": "x"})");
  const json after = json::parse(R"({"gameid": "umu-456", "kept": "x", "new": "y"})");
  const json patch = mapping::MergePatchBetween(before, after);
  CHECK(patch == json::parse(R"({"gameid": "umu-456", "store": null, "new": "y"})"));
  json applied = before;
  applied.merge_patch(patch);
  CHECK(applied == after);
}

TEST_CASE("A list setting drops empty items rather than sending them") {
  CHECK(mapping::ParseListText(R"(["", "a", ""])") == std::vector<std::string>{"a"});
  CHECK(mapping::ParseListText("").empty());
}

TEST_CASE("TypedValueFromText converts by schema type, not by looks") {
  // The schema type is authoritative: "5" under a string key must stay a
  // string, or a PATCH would change the value's JSON kind behind the user.
  CHECK(mapping::TypedValueFromText("a boolean", "true") == json(true));
  CHECK(mapping::TypedValueFromText("a boolean", "anything else") == json(false));
  CHECK(mapping::TypedValueFromText("an integer", "1500") == json(1500));
  CHECK(mapping::TypedValueFromText("a number", "0.5") == json(0.5));
  CHECK(mapping::TypedValueFromText("a string", "5") == json("5"));
  CHECK(mapping::TypedValueFromText("an array of strings", R"(["a","b"])") == json::parse(R"(["a","b"])"));
}

TEST_CASE("TypedValueFromText sends a malformed number as text") {
  // Deliberate: the server's own validator explains the bad input better
  // than a client-side guess would, so the typo is forwarded rather than
  // silently turned into 0.
  CHECK(mapping::TypedValueFromText("an integer", "twelve") == json("twelve"));
  CHECK(mapping::TypedValueFromText("a number", "") == json(""));
}

TEST_CASE("FlattenConfig produces the dotted keys the schema uses") {
  std::map<std::string, std::string> out;
  mapping::FlattenConfig(json::parse(R"({
    "auto_setup": true,
    "scan": {"debounce_ms": 1500},
    "default_runner": {"windows": "proton_umu:auto"}
  })"),
                         "", out);

  CHECK(out["auto_setup"] == "true");
  CHECK(out["scan.debounce_ms"] == "1500");
  CHECK(out["default_runner.windows"] == "proton_umu:auto");
}

TEST_CASE("FlattenConfig skips the opaque frontend table") {
  // [frontend] is passthrough storage the backend never validates; showing
  // it in a schema-generated settings screen would offer edits no schema
  // entry describes.
  std::map<std::string, std::string> out;
  mapping::FlattenConfig(json::parse(R"({
    "auto_setup": true,
    "frontend": {"window_width": 1180}
  })"),
                         "", out);

  CHECK(out.count("auto_setup") == 1);
  CHECK(out.count("frontend.window_width") == 0);
}

TEST_CASE("FlattenConfig only skips frontend at the top level") {
  // A nested key that happens to be named "frontend" is a real setting.
  std::map<std::string, std::string> out;
  mapping::FlattenConfig(json::parse(R"({"desktop_entries": {"frontend": "x"}})"), "", out);
  CHECK(out["desktop_entries.frontend"] == "x");
}

TEST_CASE("AssignDottedKey rebuilds the nesting PATCH /v1/config expects") {
  json document = json::object();
  mapping::AssignDottedKey(document, "scan.debounce_ms", json(1500));
  mapping::AssignDottedKey(document, "auto_setup", json(true));
  CHECK(document == json::parse(R"({"scan": {"debounce_ms": 1500}, "auto_setup": true})"));
}

TEST_CASE("AssignDottedKey merges two keys sharing a prefix") {
  // Both edits have to survive: writing the second must not replace the
  // object the first one created, or saving two settings in one group would
  // silently drop one of them.
  json document = json::object();
  mapping::AssignDottedKey(document, "default_runner.windows", json("proton_umu:auto"));
  mapping::AssignDottedKey(document, "default_runner.native", json("native:native"));
  CHECK(document["default_runner"].size() == 2);
  CHECK(document["default_runner"]["windows"] == "proton_umu:auto");
  CHECK(document["default_runner"]["native"] == "native:native");
}

TEST_CASE("ToApiError reads mirad's error envelope and a failure event alike") {
  const ApiError envelope = mapping::ToApiError(json::parse(
      R"({"code": "no_steamgriddb_key", "message": "needs a key", "hint": "Add one.",
          "fix": {"kind": "setting", "target": "steamgriddb.api_key"}})"));
  CHECK(envelope.message == "needs a key");
  CHECK(envelope.hint == "Add one.");
  CHECK(envelope.fix.kind == "setting");
  CHECK(envelope.fix.target == "steamgriddb.api_key");

  // game.install.failed carries the message as "error".
  const ApiError event = mapping::ToApiError(json::parse(
      R"({"id": "g", "error": "no installer", "fix": {"kind": "game", "target": "g", "step": "exe"}})"));
  CHECK(event.message == "no installer");
  CHECK(event.fix.step == "exe");

  // Both are optional.
  const ApiError bare = mapping::ToApiError(json::parse(R"({"code": "x", "message": "y", "fix": "junk"})"));
  CHECK(bare.hint.empty());
  CHECK(bare.fix.kind.empty());
}
