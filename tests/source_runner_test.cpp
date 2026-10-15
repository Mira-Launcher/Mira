#include <doctest.h>

#include "library/SourceRunner.h"
#include "runner/RunnerRegistry.h"
#include "support/TestEnv.h"

using namespace mira;

namespace {

model::Game MakeGame(const std::string& id, const std::string& source,
                     const std::string& runner_ref, const std::string& data_dir = "",
                     model::Platform platform = model::Platform::Windows) {
  model::Game game;
  game.id = id;
  game.name = id;
  game.source = source;
  game.platform = platform;
  game.runner_ref = runner_ref;
  game.data_dir = data_dir;
  return game;
}

}  // namespace

TEST_CASE("a store's runner is its default, and games follow only when asked") {
  test::TestEnv env("source-runner-store");
  REQUIRE(env.games.Upsert(MakeGame("epic-a", "epic", "")));
  REQUIRE(env.games.Upsert(MakeGame("epic-b", "epic", "wine:pinned")));
  REQUIRE(env.games.Upsert(MakeGame("epic-native", "epic", "", "", model::Platform::Native)));
  REQUIRE(env.games.Upsert(MakeGame("gog-a", "gog", "")));

  auto state = library::SetSourceRunner(env.config, env.games, "epic", "native:native", false);
  REQUIRE(state);
  CHECK(env.config.GetString("epic.runner") == "native:native");
  CHECK(state->games == 2);
  CHECK(state->differing == 2);
  CHECK(state->changed.empty());
  CHECK(env.games.Find("epic-b")->runner_ref == "wine:pinned");

  // An unpinned game resolves through the store's default.
  const runner::RunnerRegistry registry(env.config);
  CHECK(registry.ResolveRef(*env.games.Find("epic-a")) == "native:native");
  CHECK(registry.ResolveRef(*env.games.Find("gog-a")) ==
        "native:native");  // default_runner.windows

  state = library::SetSourceRunner(env.config, env.games, "epic", "native:native", true);
  REQUIRE(state);
  CHECK(state->differing == 0);
  CHECK(state->changed.size() == 2);
  CHECK(env.games.Find("epic-b")->runner_ref == "native:native");
  CHECK(env.games.Find("epic-native")->runner_ref.empty());
  CHECK(env.games.Find("gog-a")->runner_ref.empty());
}

TEST_CASE("a launcher's runner moves the launcher and the games in its prefix") {
  test::TestEnv env("source-runner-launcher");
  const std::string prefix = (env.dir / "prefixes" / "ubisoft").string();
  REQUIRE(env.games.Upsert([&] {
    model::Game host = MakeGame("launcher-ubisoft", "launcher", "wine:old", prefix);
    host.source_ref = "ubisoft";
    return host;
  }()));
  REQUIRE(env.games.Upsert(MakeGame("ubisoft-1", "ubisoft", "wine:old", prefix)));
  REQUIRE(env.games.Upsert(MakeGame("ubisoft-moved", "ubisoft", "wine:old", "/elsewhere")));

  auto state = library::GetSourceRunner(env.config, env.games, "ubisoft");
  REQUIRE(state);
  CHECK(state->runner_ref == "wine:old");
  CHECK(state->games == 1);

  state = library::SetSourceRunner(env.config, env.games, "ubisoft", "native:native", false);
  REQUIRE(state);
  CHECK(state->changed.size() == 2);
  CHECK(env.games.Find("launcher-ubisoft")->runner_ref == "native:native");
  CHECK(env.games.Find("ubisoft-1")->runner_ref == "native:native");
  CHECK(env.games.Find("ubisoft-moved")->runner_ref == "wine:old");
}

TEST_CASE("setting a source's runner refuses what it can't use") {
  test::TestEnv env("source-runner-refused");
  CHECK(library::GetSourceRunner(env.config, env.games, "steam").error().code == "no_runner");
  CHECK(library::GetSourceRunner(env.config, env.games, "ea").error().code ==
        "launcher_not_installed");
  CHECK(library::SetSourceRunner(env.config, env.games, "gog", "nonsense", false).error().code ==
        "invalid_runner");
  CHECK_FALSE(library::SetSourceRunner(env.config, env.games, "gog", "wine:not-installed", false));
  CHECK(env.config.GetString("gog.runner").empty());
  CHECK(library::SetSourceRunner(env.config, env.games, "gog", "", false));
}
