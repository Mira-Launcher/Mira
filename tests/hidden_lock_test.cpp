#include <doctest.h>
#include <httplib.h>

#include <algorithm>
#include <json.hpp>
#include <string>

#include "api/HiddenLock.h"
#include "store/GameStore.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
using test::LiveServer;
using test::TempDir;

namespace {

void AddGame(store::GameStore& games, const std::string& id, std::vector<std::string> tags) {
  model::Game game;
  game.id = id;
  game.name = id;
  game.source = "manual";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.tags = std::move(tags);
  REQUIRE(games.Upsert(game).has_value());
}

httplib::Result Send(httplib::Client& client, const std::string& method, const std::string& path,
                     const json& body = nullptr) {
  const std::string text = body.is_null() ? "" : body.dump();
  if (method == "PUT") return client.Put(path, text, "application/json");
  if (method == "PATCH") return client.Patch(path, text, "application/json");
  if (method == "POST") return client.Post(path, text, "application/json");
  return client.Get(path);
}

json Answer(httplib::Client& client, const std::string& method, const std::string& path,
            const json& body = nullptr, int status = 200) {
  const auto res = Send(client, method, path, body);
  REQUIRE(res != nullptr);
  INFO(method, " ", path, ": ", res->body);
  REQUIRE(res->status == status);
  return json::parse(res->body);
}

std::vector<std::string> GameIds(httplib::Client& client, const std::string& query) {
  std::vector<std::string> ids;
  for (const json& game : Answer(client, "GET", "/v1/games" + query)) ids.push_back(game["id"]);
  std::ranges::sort(ids);
  return ids;
}

bool Mentions(const api::EventBus& events, const std::string& id) {
  return std::ranges::any_of(events.Since(0), [&](const model::Event& event) {
    return event.payload.dump().find('"' + id + '"') != std::string::npos;
  });
}

}  // namespace

TEST_CASE(
    "Locked hidden games are left out of every answer and event until the password is given") {
  LiveServer server(TempDir("hidden-lock-state"));
  AddGame(server.games(), "seen", {"shared"});
  AddGame(server.games(), "secret", {"shared", "hidden"});
  REQUIRE(server.games().SetTitlesHidden({"steam-99"}, true).has_value());
  httplib::Client client = server.Client();

  // No password: never locked, and hidden games are there to ask for.
  CHECK(Answer(client, "GET", "/v1/hidden") == json{{"password", false}, {"locked", false}});
  CHECK(std::ranges::contains(GameIds(client, "?include_hidden=true"), "secret"));
  server.events().Publish("game.updated", {{"id", "secret"}, {"name", "secret"}});

  CHECK(Answer(client, "PUT", "/v1/hidden/password", {{"password", "hunter2"}})["locked"] == false);
  CHECK(Answer(client, "POST", "/v1/hidden/lock") == json{{"password", true}, {"locked", true}});
  // What was published before the lock is blanked, not replayed to a client that reconnects.
  CHECK_FALSE(Mentions(server.events(), "secret"));

  // Lists leave it out whatever is asked for, and naming it answers as for a game that isn't there.
  CHECK(GameIds(client, "?include_hidden=true") == std::vector<std::string>{"seen"});
  CHECK(GameIds(client, "?tag=hidden").empty());
  const json absent = Answer(client, "GET", "/v1/games/nope", nullptr, 404);
  CHECK(Answer(client, "GET", "/v1/games/secret", nullptr, 404) == absent);
  const json tags = Answer(client, "GET", "/v1/tags")["tags"];
  REQUIRE(tags.size() == 1);
  CHECK(tags[0]["ids"] == json::array({"seen"}));

  // It can't be changed or unhidden, and setting a tag on exactly the listed games leaves it alone.
  Answer(client, "PATCH", "/v1/games", {{"ids", {"secret"}}, {"remove_tags", {"hidden"}}}, 404);
  Answer(client, "POST", "/v1/library/hidden",
         {{"source", "steam"}, {"refs", {"99"}}, {"hidden", false}}, 404);
  Answer(client, "POST", "/v1/tags/set", {{"name", "shared"}, {"ids", json::array()}});
  CHECK(server.games().Find("secret")->tags == std::vector<std::string>{"shared", "hidden"});
  CHECK(server.games().HiddenTitles().contains("steam-99"));
  CHECK(server.games().Find("seen")->tags.empty());

  // Hiding a game while locked takes it away from clients.
  Answer(client, "PATCH", "/v1/games", {{"ids", {"seen"}}, {"add_tags", {"hidden"}}});
  CHECK(GameIds(client, "").empty());
  const auto removed = test::WaitForEvent(server.events(), "games.removed");
  REQUIRE(removed);
  CHECK(removed->payload["ids"] == json::array({"seen"}));
  CHECK_FALSE(Mentions(server.events(), "secret"));

  // A wrong password changes nothing; the right one brings everything back.
  CHECK(Answer(client, "POST", "/v1/hidden/unlock", {{"password", "hunter3"}},
               403)["error"]["code"] == "wrong_password");
  CHECK(GameIds(client, "?include_hidden=true").empty());
  CHECK(Answer(client, "POST", "/v1/hidden/unlock", {{"password", "hunter2"}})["locked"] == false);
  CHECK(GameIds(client, "?include_hidden=true") == std::vector<std::string>{"secret", "seen"});
  CHECK(Answer(client, "GET", "/v1/games/secret")["id"] == "secret");
  const auto changed = test::WaitForEvent(server.events(), "hidden.changed");
  REQUIRE(changed);
}

TEST_CASE("The password survives a restart, and changing or removing it needs the current one") {
  LiveServer server(TempDir("hidden-password-state"));
  httplib::Client client = server.Client();
  Answer(client, "PUT", "/v1/hidden/password", {{"password", "first"}});

  // A new mirad starts locked, and keeps no plain copy.
  api::HiddenLock restarted(server.games());
  CHECK(restarted.Locked());
  CHECK(server.games().Secret("hidden_password").find("first") == std::string::npos);
  CHECK_FALSE(restarted.Unlock("second").has_value());
  CHECK(restarted.Unlock("first").has_value());

  Answer(client, "PUT", "/v1/hidden/password", {{"password", "second"}}, 403);
  Answer(client, "PUT", "/v1/hidden/password", {{"current", "first"}, {"password", "second"}});
  CHECK_FALSE(api::HiddenLock(server.games()).Unlock("first").has_value());
  CHECK(api::HiddenLock(server.games()).Unlock("second").has_value());

  // Leaving hidden mode locks only while hidden.lock_on_leave is on.
  REQUIRE(server.MutableConfig().Set("hidden.lock_on_leave", false).has_value());
  CHECK(Answer(client, "POST", "/v1/hidden/left")["locked"] == false);
  REQUIRE(server.MutableConfig().Set("hidden.lock_on_leave", true).has_value());
  CHECK(Answer(client, "POST", "/v1/hidden/left")["locked"] == true);
  Answer(client, "POST", "/v1/hidden/unlock", {{"password", "second"}});

  CHECK(Answer(client, "PUT", "/v1/hidden/password", {{"current", "second"}, {"password", ""}}) ==
        json{{"password", false}, {"locked", false}});
  CHECK(Answer(client, "POST", "/v1/hidden/lock")["locked"] == false);
  CHECK_FALSE(api::HiddenLock(server.games()).Locked());
}
