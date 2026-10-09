#include <doctest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "config/Config.h"
#include "runner/Curl.h"
#include "runner/Exec.h"
#include "runner/NativeRunner.h"
#include "runner/RefMigration.h"
#include "runner/RunnerRegistry.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {
fs::path TempFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests";
  fs::create_directories(dir);
  const fs::path file = dir / name;
  fs::remove(file);
  return file;
}
}  // namespace

TEST_CASE("NativeRunner builds argv from install_path/exe_path and splits args") {
  model::Game game;
  game.install_path = "/games/Celeste";
  game.exe_path = "Celeste";
  game.args = "-fullscreen -novideo";
  game.env["FOO"] = "bar";

  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  CHECK(command->argv == std::vector<std::string>{"/games/Celeste/Celeste", "-fullscreen", "-novideo"});
  CHECK(command->cwd == "/games/Celeste");
  CHECK(command->env.at("FOO") == "bar");
}

TEST_CASE("NativeRunner runs a .sh through sh, with or without its execute bit") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "native-sh";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path script = dir / "start.sh";
  std::ofstream(script) << "#!/bin/sh\necho hi\n";

  model::Game game;
  game.install_path = dir.string();
  game.exe_path = "start.sh";
  runner::NativeRunner native;
  for (const fs::perms perms :
       {fs::perms::owner_read | fs::perms::owner_write, fs::perms::owner_all}) {
    fs::permissions(script, perms);
    auto command = native.BuildCommand(game, std::nullopt);
    REQUIRE(command.has_value());
    CHECK(command->argv == std::vector<std::string>{"sh", script.string()});
  }
  fs::remove_all(dir);
}

TEST_CASE("NativeRunner rejects a non-script exe with no execute bit") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "native-noexec";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path exe = dir / "game";
  std::ofstream(exe) << "not actually elf, doesn't matter here";
  fs::permissions(exe, fs::perms::owner_read | fs::perms::owner_write);

  model::Game game;
  game.install_path = dir.string();
  game.exe_path = "game";
  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE_FALSE(command.has_value());
  CHECK(command.error().code == "not_executable");
  fs::remove_all(dir);
}

TEST_CASE("NativeRunner runs an executable AppImage directly when FUSE is available") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "native-appimage";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path exe = dir / "Game.AppImage";
  std::ofstream(exe) << "not a real appimage, doesn't matter here";
  fs::permissions(exe, fs::perms::owner_all);

  model::Game game;
  game.install_path = dir.string();
  game.exe_path = "Game.AppImage";
  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  // Whether extraction is forced depends on whether this machine actually has
  // FUSE: assert whichever shape that implies, same posture as the
  // environment-dependent Proton/Wine tests below.
  if (command->argv.size() > 1 && command->argv[1] == "--appimage-extract-and-run") {
    CHECK(command->argv[0] == exe.string());
  } else {
    CHECK(command->argv == std::vector<std::string>{exe.string()});
  }
  fs::remove_all(dir);
}

TEST_CASE("NativeRunner auto-chmods an AppImage missing its execute bit instead of failing") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "native-appimage-noexec";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path exe = dir / "Game.AppImage";
  std::ofstream(exe) << "not a real appimage, doesn't matter here";
  fs::permissions(exe, fs::perms::owner_read | fs::perms::owner_write);  // no +x -- as a browser download leaves it

  model::Game game;
  game.install_path = dir.string();
  game.exe_path = "Game.AppImage";
  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  CHECK(command->argv[0] == exe.string());

  std::error_code ec;
  CHECK((fs::status(exe, ec).permissions() & fs::perms::owner_exec) != fs::perms::none);
  fs::remove_all(dir);
}

TEST_CASE("RunnerRegistry::Resolve rejects a malformed or unknown reference") {
  config::Config config(TempFile("runner-registry-bad.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  CHECK_FALSE(registry.Resolve("no-colon-here").has_value());
  CHECK_FALSE(registry.Resolve("not_a_real_kind:whatever").has_value());
}

TEST_CASE("DeduplicateBuilds keeps the first of one build found twice") {
  // On a normal Steam setup ~/.steam/steam is a symlink to
  // ~/.local/share/Steam and both get searched, so every Proton build there
  // shows up once per path.
  const fs::path root = fs::temp_directory_path() / "mira-tests" / "runner-dedupe";
  fs::remove_all(root);
  const fs::path real = root / "real" / "GE-Proton11-7";
  fs::create_directories(real);
  const fs::path link = root / "link";
  std::error_code ec;
  fs::create_directory_symlink(root / "real", link, ec);
  REQUIRE_FALSE(ec);

  const std::vector<model::RunnerBuild> unique = runner::DeduplicateBuilds({
      {"proton", "GE-Proton11-7", real.string(), "2", "GE-Proton11-7"},
      {"proton", "GE-Proton11-7", (link / "GE-Proton11-7").string(), "2", "GE-Proton11-7"},
  });
  REQUIRE(unique.size() == 1);
  CHECK(unique[0].path == real.string());
  fs::remove_all(root);

  // Different directories with the same "kind:name": that is all a game
  // stores, so no client could pick between them.
  CHECK(runner::DeduplicateBuilds({
                                      {"proton", "GE-Proton11-7", "/a/GE-Proton11-7", "2", "GE-Proton11-7"},
                                      {"proton", "GE-Proton11-7", "/b/GE-Proton11-7", "2", "GE-Proton11-7"},
                                  })
            .size() == 1);
}

TEST_CASE("DeduplicateBuilds keeps genuinely different builds, in order") {
  std::vector<model::RunnerBuild> builds = {
      {"proton", "GE-Proton11-7", "/a/GE-Proton11-7", "2", "GE-Proton11-7"},
      {"proton", "GE-Proton11-6", "/a/GE-Proton11-6", "1", "GE-Proton11-6"},
      {"wine", "system", "/usr/bin/wine", "wine-11.17", "system"},
  };
  const std::vector<model::RunnerBuild> unique = runner::DeduplicateBuilds(builds);
  REQUIRE(unique.size() == 3);
  CHECK(unique[0].name == "GE-Proton11-7");  // newest-first order preserved
  CHECK(unique[2].kind == "wine");
}

TEST_CASE("ProvisionGame marks a native game ready with no build") {
  config::Config config(TempFile("runner-registry-native-provision.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  model::Game game;
  game.id = "celeste";
  game.platform = model::Platform::Native;
  game.install_path = "/games/Celeste";
  game.exe_path = "Celeste";

  model::Game result = registry.ProvisionGame(game);
  CHECK(result.status == model::GameStatus::Ready);
  CHECK(result.runner_ref == "native:native");
}

TEST_CASE("Resolve reports an uninstalled build instead of succeeding with none") {
  config::Config config(TempFile("resolve-missing-build.toml"));
  config.Load();
  // Point discovery at nothing, so no wine/proton builds exist at all.
  REQUIRE(config.Set("runner_search_paths", nlohmann::json::array()).has_value());
  REQUIRE(config.Set("wine_search_paths", nlohmann::json::array()).has_value());
  runner::RunnerRegistry registry(config);

  // Regression: this used to return success-with-no-build, which made a
  // Windows game resolve to "no build" and report a vague error, and made
  // native:anything-at-all silently "succeed" for a Windows game.
  auto proton = registry.Resolve("proton:GE-Proton-Nonexistent");
  CHECK_FALSE(proton.has_value());

  // native has no build concept at all, so it still resolves.
  auto native = registry.Resolve("native:native");
  REQUIRE(native.has_value());
  CHECK_FALSE(native->build.has_value());
}

TEST_CASE("ResolveRef leaves a non-empty runner_ref untouched") {
  config::Config config(TempFile("resolveref-explicit.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  model::Game game;
  game.platform = model::Platform::Windows;
  game.runner_ref = "wine:system";
  CHECK(registry.ResolveRef(game) == "wine:system");
}

TEST_CASE("ResolveRef falls back to default_runner.native for an empty ref on a native game") {
  config::Config config(TempFile("resolveref-native.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  model::Game game;
  game.platform = model::Platform::Native;
  CHECK(registry.ResolveRef(game) == "native:native");
}

TEST_CASE("ResolveRef expands \"auto\" to a real installed windows runner, not native") {
  // Regression: /launch and /run used to hardcode native:native for any
  // empty runner_ref, silently ignoring default_runner.windows entirely
  // (including its "auto" case) for every Windows game with no runner_ref
  // set yet -- e.g. a fresh Lutris import.
  config::Config config(TempFile("resolveref-windows-auto.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  model::Game game;
  game.platform = model::Platform::Windows;
  const std::string ref = registry.ResolveRef(game);
  CHECK(ref != "native:native");

  CHECK((ref == "proton:auto" || ref == "wine:auto"));

  // auto prefers a distro package, then the preferred source, then the newest.
  const model::RunnerBuild packaged{
      .kind = "wine", .name = "system", .path = "/usr/bin/wine", .version = "wine-9.0", .release = "system"};
  const model::RunnerBuild tkg{.kind = "wine", .name = "wine-11.17-staging-tkg-amd64",
                               .path = "/home/u/w/wine-11.17-staging-tkg-amd64/bin/wine", .version = "wine-11.17",
                               .release = "wine-11.17-staging-tkg-amd64"};
  const model::RunnerBuild vanilla{.kind = "wine", .name = "wine-11.18-amd64",
                                   .path = "/home/u/w/wine-11.18-amd64/bin/wine", .version = "wine-11.18",
                                   .release = "wine-11.18-amd64"};
  CHECK(runner::PickAuto(config, {tkg, packaged, vanilla}).name == "system");
  CHECK(runner::PickAuto(config, {vanilla, tkg}).name == tkg.name);
}

TEST_CASE("the old proton_umu: runner_ref spelling still resolves after the rename") {
  // umu was briefly modelled as its own runner kind; it's the mechanism
  // Proton runs through, not a runner. A games.toml written before the
  // rename must keep working.
  config::Config config(TempFile("legacy-ref.toml"));
  config.Load();
  const fs::path runners = fs::temp_directory_path() / "mira-tests" / "legacy-ref-runners";
  fs::remove_all(runners);
  fs::create_directories(runners / "Fake-Proton-1");
  std::ofstream(runners / "Fake-Proton-1" / "proton").close();
  std::ofstream(runners / "Fake-Proton-1" / "toolmanifest.vdf").close();
  std::ofstream(runners / "Fake-Proton-1" / "version") << "1700000000 Fake-Proton-1";
  REQUIRE(config.Set("runner_search_paths", nlohmann::json::array({runners.string()})));
  REQUIRE(config.Set("runner_scan_common_dirs", false));
  runner::RunnerRegistry registry(config);

  auto legacy = registry.Resolve("proton_umu:latest");
  REQUIRE(legacy.has_value());
  CHECK(legacy->runner->kind() == "proton");
}

TEST_CASE("A Proton build Steam updates in place keeps its reference across updates, and old ones move to it") {
  test::TestEnv env("in-place-runner");
  REQUIRE(env.config.Set("runner_scan_common_dirs", false));
  // Steam's own Proton lives in steamapps/common and is replaced by each update.
  const fs::path common = env.dir / "steamapps" / "common";
  const fs::path tools = env.dir / "compatibilitytools.d";
  REQUIRE(env.config.Set("runner_search_paths", nlohmann::json::array({common.string(), tools.string()})));
  const auto install = [](const fs::path& dir, const std::string& release) {
    fs::create_directories(dir);
    std::ofstream(dir / "proton").close();
    std::ofstream(dir / "toolmanifest.vdf").close();
    std::ofstream(dir / "version") << "1700000000 " << release;
  };
  install(common / "Proton 11.0", "proton-11.0-3-x86_64");
  install(tools / "GE-Proton10-4", "GE-Proton10-4");

  model::Game pinned;
  pinned.id = "pinned";
  pinned.runner_ref = "proton:proton-11.0-2c-x86_64";  // the release before Steam's update
  pinned.overrides = {{"launchers.runner", "proton:proton-11.0-2c-x86_64"}};
  model::Game ge;
  ge.id = "ge";
  ge.runner_ref = "proton:GE-Proton10-4";
  REQUIRE(env.games.Upsert(pinned).has_value());
  REQUIRE(env.games.Upsert(ge).has_value());
  REQUIRE(env.config.Set("gog.runner", "proton:proton-11.0-2c-x86_64"));

  CHECK(runner::MigrateInPlaceRefs(env.config, env.games) == 2);
  CHECK(env.games.Find("pinned")->runner_ref == "proton:Proton 11.0");
  CHECK(env.games.Find("pinned")->overrides["launchers.runner"] == "proton:Proton 11.0");
  CHECK(env.games.Find("ge")->runner_ref == "proton:GE-Proton10-4");
  CHECK(env.config.GetString("gog.runner") == "proton:Proton 11.0");

  // The next update changes the release inside, not the reference.
  install(common / "Proton 11.0", "proton-11.0-4-x86_64");
  const runner::RunnerRegistry registry(env.config);
  const auto resolved = registry.Resolve(env.games.Find("pinned")->runner_ref);
  REQUIRE(resolved.has_value());
  CHECK(resolved->build->release == "proton-11.0-4-x86_64");
}

TEST_CASE("Steam's Proton is found in a library on another drive") {
  test::TestEnv env("steam-library-proton");
  const fs::path root = env.dir / "steam";
  const fs::path library = env.dir / "games-drive";
  test::Touch(root / "steamapps" / "libraryfolders.vdf",
              "\"libraryfolders\"\n{\n \"0\" { \"path\" \"" + root.string() + "\" }\n \"1\" { \"path\" \"" +
                  library.string() + "\" }\n}\n");
  const fs::path proton = library / "steamapps" / "common" / "Proton 9.0";
  test::Touch(proton / "proton");
  test::Touch(proton / "toolmanifest.vdf");
  REQUIRE(env.config.Set("steam.root", root.string()));
  REQUIRE(env.config.Set("runner_scan_common_dirs", true));

  const runner::RunnerRegistry registry(env.config);
  CHECK(registry.Resolve("proton:Proton 9.0").has_value());
}

TEST_CASE("An old Proton reference moves only to the build of its own version") {
  test::TestEnv env("in-place-runner-versions");
  REQUIRE(env.config.Set("runner_scan_common_dirs", false));
  const fs::path common = env.dir / "steamapps" / "common";
  REQUIRE(env.config.Set("runner_search_paths", nlohmann::json::array({common.string()})));
  const auto install = [](const fs::path& dir, const std::string& release) {
    fs::create_directories(dir);
    std::ofstream(dir / "proton").close();
    std::ofstream(dir / "toolmanifest.vdf").close();
    std::ofstream(dir / "version") << "1700000000 " << release;
  };
  const auto game = [&](const std::string& id, const std::string& ref) {
    model::Game g;
    g.id = id;
    g.runner_ref = ref;
    REQUIRE(env.games.Upsert(g).has_value());
  };
  install(common / "Proton 8.0", "proton-8.0-5-x86_64");
  install(common / "Proton 9.0", "proton-9.0-3-x86_64");
  install(common / "proton-cachyos-slr", "cachyos-11.0-20261005-slr");
  game("nine", "proton:proton-9.0-2-x86_64");
  game("seven", "proton:proton-7.0-6-x86_64");               // Proton 7.0 is gone
  game("cachyos", "proton:cachyos-10.0-20260703-slr");  // the package moved to 11

  runner::MigrateInPlaceRefs(env.config, env.games);
  CHECK(env.games.Find("nine")->runner_ref == "proton:Proton 9.0");
  CHECK(env.games.Find("seven")->runner_ref == "proton:proton-7.0-6-x86_64");
  CHECK(env.games.Find("cachyos")->runner_ref == "proton:proton-cachyos-slr");
}

TEST_CASE("NativeRunner runs an absolute exe_path as-is, in its own folder, whatever install_path is") {
  model::Game game;
  game.install_path = "/nonexistent/.var/app/com.example.App";
  game.exe_path = "/usr/bin/flatpak";
  game.args = "run com.example.App";

  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  CHECK(command->argv == std::vector<std::string>{"/usr/bin/flatpak", "run", "com.example.App"});
  CHECK(command->cwd == "/usr/bin");
}

TEST_CASE("RunAndWait kills a timed-out command and what it spawned") {
  mira::Command command;
  command.argv = {"sh", "-c", "sleep 30 & sleep 30"};
  command.timeout_s = 1;
  const auto started = std::chrono::steady_clock::now();
  const auto result = mira::runner::RunAndWait(command);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == "exec_timeout");
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
}

TEST_CASE("SpawnDetached gives the child /dev/null, not mirad's own pipes, which may have no reader left") {
  // Mirad can outlive the window that started it, leaving its stdout and stderr as pipes nobody reads. A launch that
  // inherited them died on its first write.
  int broken[2];
  REQUIRE(::pipe(broken) == 0);
  ::close(broken[0]);  // nobody reads
  const int saved_out = ::dup(1);
  const int saved_err = ::dup(2);
  ::signal(SIGPIPE, SIG_IGN);
  ::dup2(broken[1], 1);
  ::dup2(broken[1], 2);

  Command command;
  command.argv = {"sh", "-c", "echo out; echo err >&2"};
  const auto pid = runner::SpawnDetached(command);
  int status = -1;
  if (pid) ::waitpid(*pid, &status, 0);

  ::dup2(saved_out, 1);
  ::dup2(saved_err, 2);
  ::close(saved_out);
  ::close(saved_err);
  ::close(broken[1]);

  REQUIRE(pid.has_value());
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("CurlDownload retries through a dropped network instead of failing on the first error") {
  const Command command = runner::CurlDownloadCommand("https://example.invalid/file", "/tmp/mira-tests/file");
  const auto has = [&](const char* flag) { return std::ranges::find(command.argv, flag) != command.argv.end(); };
  CHECK(has("--retry"));
  CHECK(has("--retry-all-errors"));  // a dropped network is a timeout, a reset or a DNS failure
  CHECK(has("--retry-max-time"));    // but not forever
  CHECK(has("-sSLf"));  // never saves an HTTP error page as the file
}
