#include <doctest.h>

#include <filesystem>
#include <format>

#include "config/Config.h"
#include "core/Files.h"
#include "saves/Backups.h"
#include "saves/remote/Apply.h"
#include "saves/remote/Rclone.h"
#include "saves/remote/RemoteConfig.h"
#include "saves/remote/RemoteSync.h"
#include "saves/remote/Revisions.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("rclone counts from 1.76 and is found from the setting, then PATH, when new enough") {
  CHECK(saves::remote::NewEnough("rclone v1.76.0\n- os/version: linux"));
  CHECK(saves::remote::NewEnough("v1.77.2"));
  CHECK(saves::remote::NewEnough("v2.0.0"));
  CHECK_FALSE(saves::remote::NewEnough("v1.60.1-DEV"));
  CHECK_FALSE(saves::remote::NewEnough("v1.75.9"));
  CHECK_FALSE(saves::remote::NewEnough(""));
  CHECK_FALSE(saves::remote::NewEnough("rclone unknown"));

  const fs::path dir = test::TempDir("remote-saves-rclone");
  config::Config config(dir / "settings.toml");
  config.Load();
  test::Isolate(config);

  const fs::path fresh = dir / "fresh" / "rclone";
  test::Touch(fresh, "#!/bin/sh\necho 'rclone v1.76.0'\n", true);
  REQUIRE(config.Set("saves.rclone_bin", fresh.string()));
  const runner::ToolStatus found = saves::remote::DetectRclone(config);
  CHECK(found.installed);
  CHECK(found.source == "override");

  const fs::path old = dir / "old" / "rclone";
  test::Touch(old, "#!/bin/sh\necho 'rclone v1.60.1-DEV'\n", true);
  REQUIRE(config.Set("saves.rclone_bin", old.string()));
  CHECK_FALSE(saves::remote::DetectRclone(config).installed);

  REQUIRE(config.Set("saves.rclone_bin", ""));
  const test::PathPrepend path(dir / "old");
  CHECK_FALSE(saves::remote::DetectRclone(config).installed);
}

namespace {

using saves::remote::Revision;
using saves::remote::RevisionFile;

const std::string kSum(64, 'a');

// An rclone whose remote "box:" is a folder; with <dir>/offline present it can't be reached.
void UseFakeRclone(config::Config& config, const fs::path& dir) {
  const fs::path script = dir / "bin" / "rclone";
  test::Touch(script,
              std::format(R"sh(#!/bin/sh
[ "$1" = "--version" ] && {{ echo 'rclone v1.76.0'; exit 0; }}
echo "$* [$RCLONE_CONFIG]" >> '{0}/calls'
case "$*" in *" obscure -") pw="$(cat)"; echo "obscured_${{#pw}}"; exit 0 ;; esac
[ -e '{0}/offline' ] && exit 5
at() {{ case "$1" in box:*|primary:*) echo "{0}/box/${{1#*:}}" ;; mirror:*) echo "{0}/mirror/${{1#*:}}" ;; *) echo "$1" ;; esac; }}
case "$*" in *--files-from-raw*--max-size*) exit 1 ;; esac  # as rclone does
from="$(at "$2")"
case "$1" in
  copy)
    to="$(at "$3")"; [ -d "$from" ] || exit 3; mkdir -p "$to"
    if [ "$4" = "--include" ]; then
      for m in "$from"/*/manifest.json; do
        [ -f "$m" ] || continue
        name="$(basename "$(dirname "$m")")"; mkdir -p "$to/$name"; cp "$m" "$to/$name/"
      done
    else cp -Rp "$from/." "$to/"; fi ;;
  copyto) to="$(at "$3")"; mkdir -p "$(dirname "$to")"; cp "$from" "$to" ;;
  deletefile) rm "$from" 2>/dev/null || exit 4 ;;
  purge) [ -d "$from" ] || exit 3; rm -r "$from" ;;
esac
)sh",
                          dir.string()),
              true);
  REQUIRE(config.Set("saves.rclone_bin", script.string()));
}

}  // namespace

TEST_CASE("a manifest from a remote is refused unless every path in it is safe") {
  const Revision good{.id = "r1", .files = {{"prefix_user", "Saves/a.sav", 3, 1, kSum}}};
  const auto parse = [&](const auto& change) {
    Revision revision = good;
    change(revision);
    return saves::remote::ParseManifest(saves::remote::ManifestJson(revision).dump(), "r1");
  };
  const auto read = parse([](Revision&) {});
  REQUIRE(read);
  CHECK(read->files == good.files);
  for (const char* path : {"../x", "/etc/passwd", "a/../../x", "a//b", "", "a/.", "a\nb"}) {
    CAPTURE(path);
    CHECK_FALSE(parse([&](Revision& r) { r.files[0].path = path; }));
  }
  CHECK_FALSE(parse([](Revision& r) { r.files[0].root = ".."; }));
  CHECK_FALSE(parse([](Revision& r) { r.files[0].sha256 = "none"; }));
  CHECK_FALSE(parse([](Revision& r) { r.id = "r2"; }));
  CHECK_FALSE(parse([](Revision& r) { r.parents = {"../r0"}; }));
  CHECK_FALSE(
      parse([](Revision& r) { r.files.push_back({"prefix_user", "saves/A.SAV", 3, 1, kSum}); }));
  CHECK_FALSE(parse(
      [](Revision& r) { r.files.push_back({"prefix_user", "Saves/a.sav/inner", 3, 1, kSum}); }));
  CHECK_FALSE(saves::remote::ParseManifest("not json", "r1"));
}

TEST_CASE("two uploads from one revision are two leaves, and only a single leaf is pruned under") {
  std::vector<Revision> all = {{.id = "a", .taken_at = 9},
                               {.id = "b", .parents = {"a"}, .taken_at = 2},
                               {.id = "c", .parents = {"b"}, .taken_at = 1}};
  REQUIRE(saves::remote::Leaves(all).size() == 1);
  CHECK(saves::remote::Leaves(all)[0].id == "c");
  CHECK(saves::remote::Prunable(all, 2) == std::vector<std::string>{"a"});
  CHECK(saves::remote::Prunable(all, 0) == std::vector<std::string>{"a", "b"});

  all.push_back({.id = "d", .parents = {"b"}, .taken_at = 3});
  CHECK(saves::remote::Leaves(all).size() == 2);
  CHECK(saves::remote::Prunable(all, 1).empty());
  all.push_back({.id = "e", .parents = {"c", "d"}, .taken_at = 4});
  CHECK(saves::remote::Leaves(all).size() == 1);
}

TEST_CASE("a revision goes to the remote and back, and a damaged or unfinished one is refused") {
  const fs::path dir = test::TempDir("remote-revisions");
  config::Config config(dir / "settings.toml");
  config.Load();
  test::Isolate(config);
  UseFakeRclone(config, dir);
  const test::EnvVar inherited("RCLONE_CONFIG", "/elsewhere");
  const saves::remote::Remote remote{
      .config_file = dir / "rclone.conf", .root = "box:", .ca_file = {}};

  auto none = saves::remote::ListRevisions(config, remote, "gog-1");
  REQUIRE(none);
  CHECK(none->empty());

  const fs::path staged = dir / "up";
  test::Touch(staged / "files/prefix_user/Saves/a b.sav", "save");
  const auto sums = saves::remote::Sha256({staged / "files/prefix_user/Saves/a b.sav"});
  REQUIRE(sums);
  const Revision revision{.id = saves::remote::NewRevisionId(1'800'000'000),
                          .device_name = "desk",
                          .files = {{"prefix_user", "Saves/a b.sav", 4, 1, (*sums)[0]}}};
  REQUIRE(saves::remote::UploadRevision(config, remote, "gog-1", revision, staged));
  test::Touch(dir / "box/mira-saves/v1/gog-1/unfinished/files/home/x", "half");

  const auto listed = saves::remote::ListRevisions(config, remote, "gog-1");
  REQUIRE(listed);
  REQUIRE(listed->size() == 1);
  CHECK(saves::remote::SameFiles((*listed)[0].files, revision.files));
  fs::create_directories(dir / "down");
  REQUIRE(saves::remote::DownloadRevision(config, remote, "gog-1", (*listed)[0], dir / "down"));
  CHECK(files::ReadFile(dir / "down/files/prefix_user/Saves/a b.sav") == "save");
  CHECK(files::ReadFile(dir / "calls")->find("/elsewhere") == std::string::npos);

  test::Touch(dir / "box/mira-saves/v1/gog-1" / revision.id / "files/prefix_user/Saves/a b.sav",
              "evil");
  fs::create_directories(dir / "bad");
  const auto damaged =
      saves::remote::DownloadRevision(config, remote, "gog-1", revision, dir / "bad");
  REQUIRE_FALSE(damaged);
  CHECK(damaged.error().code == "remote_damaged");

  REQUIRE(saves::remote::DeleteRevision(config, remote, "gog-1", revision.id));
  CHECK(saves::remote::ListRevisions(config, remote, "gog-1")->empty());
  CHECK_FALSE(saves::remote::ListRevisions(config, remote, "../gog-1"));
  test::Touch(dir / "offline");
  const auto offline = saves::remote::ListRevisions(config, remote, "gog-1");
  REQUIRE_FALSE(offline);
  CHECK(offline.error().code == "remote_offline");
}

TEST_CASE("a remote request is checked before anything is stored") {
  using Values = std::map<std::string, std::string>;
  const fs::path dir = test::TempDir("remote-config-check");
  test::Touch(dir / "key", "key");
  test::Touch(dir / "known_hosts", "host");
  const std::string key = (dir / "key").string();
  const std::string known = (dir / "known_hosts").string();
  const auto make = [](std::string provider, Values values, std::string folder = "saves") {
    return saves::remote::RemoteRequest{.slot = "primary",
                                        .provider = std::move(provider),
                                        .name = "Home",
                                        .folder = std::move(folder),
                                        .ca_file = {},
                                        .values = std::move(values)};
  };
  const auto with = [](Values base, const char* key, std::string value) {
    base[key] = std::move(value);
    return base;
  };
  const Values dav = {
      {"url", "https://dav.example.com/remote.php"}, {"user", "me"}, {"password", "secret"}};
  const Values sftp = {{"host", "nas.example.com"},
                       {"port", "2222"},
                       {"user", "me"},
                       {"key_file", key},
                       {"known_hosts_file", known}};
  const Values s3 = {{"endpoint", "https://s3.example.com"},
                     {"region", "eu"},
                     {"access_key_id", "AK"},
                     {"secret_access_key", "SK"}};

  CHECK(saves::remote::CheckRequest(make("webdav", dav)));
  CHECK(saves::remote::CheckRequest(make("sftp", sftp)));
  CHECK(saves::remote::CheckRequest(make("s3", s3, "bucket/saves")));
  CHECK(saves::remote::CheckRequest(make("webdav", with(dav, "url", "HTTPS://dav.example.com"))));

  CHECK_FALSE(
      saves::remote::CheckRequest(make("webdav", with(dav, "url", "http://dav.example.com"))));
  CHECK_FALSE(
      saves::remote::CheckRequest(make("webdav", with(dav, "url", "https://me@dav.example.com"))));
  CHECK_FALSE(saves::remote::CheckRequest(make("webdav", with(dav, "user", "me\nx"))));
  CHECK_FALSE(
      saves::remote::CheckRequest(make("webdav", with(dav, "no_check_certificate", "true"))));
  Values no_known = sftp;
  no_known.erase("known_hosts_file");
  CHECK_FALSE(saves::remote::CheckRequest(make("sftp", no_known)));
  CHECK_FALSE(saves::remote::CheckRequest(make("sftp", with(sftp, "host", "-oProxyCommand"))));
  CHECK_FALSE(
      saves::remote::CheckRequest(make("sftp", with(sftp, "known_hosts_file", "known_hosts"))));
  CHECK_FALSE(saves::remote::CheckRequest(make("webdav", with(dav, "user", "`me"))));
  CHECK(saves::remote::CheckRequest(make("webdav", with(dav, "password", " \"pass "))));
  CHECK_FALSE(
      saves::remote::CheckRequest(make("s3", with(s3, "endpoint", "http://s3.example.com"), "b")));
  CHECK_FALSE(saves::remote::CheckRequest(make("ftp", dav)));
  CHECK_FALSE(saves::remote::CheckRequest(make("webdav", dav, "../x")));
}

TEST_CASE(
    "a remote is saved beside other rclone sections, found, forgotten and kept off the logs") {
  const fs::path dir = test::TempDir("remote-config-save");
  config::Config config(dir / "settings.toml");
  config.Load();
  test::Isolate(config);
  UseFakeRclone(config, dir);
  const fs::path conf = dir / "rclone.conf";
  const fs::path remotes = dir / "remotes.json";
  test::Touch(conf, "[other]\nx = 1\n");

  saves::remote::RemoteRequest request{.slot = "primary",
                                       .provider = "webdav",
                                       .name = "Home",
                                       .folder = "saves",
                                       .ca_file = {},
                                       .values = {{"url", "https://dav.example.com/remote.php"},
                                                  {"user", "me"},
                                                  {"password", "hunter2-secret"}}};
  const auto saved = saves::remote::SaveRemote(config, request);
  REQUIRE(saved);
  CHECK(saved->name == "Home");
  for (const fs::path& file : {conf, remotes})
    CHECK((fs::status(file).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write));

  const std::string text = files::ReadFile(conf).value_or("");
  CHECK(text.starts_with("[other]\nx = 1\n"));
  CHECK(text.find("[primary]") != std::string::npos);
  CHECK(text.find("hunter2-secret") == std::string::npos);
  CHECK(files::ReadFile(remotes).value_or("").find("hunter2-secret") == std::string::npos);
  CHECK(files::ReadFile(dir / "calls").value_or("").find("hunter2-secret") == std::string::npos);

  const auto found = saves::remote::FindRemote(config, "primary");
  REQUIRE(found);
  CHECK(found->id == saved->id);
  CHECK(found->provider == "webdav");
  CHECK_FALSE(saves::remote::FindRemote(config, "mirror"));
  const saves::remote::Remote remote = saves::remote::RemoteFor(config, *found);
  CHECK(remote.config_file == conf);
  CHECK(remote.root == "primary:saves");

  request.values["user"] = "you";
  REQUIRE(saves::remote::SaveRemote(config, request));
  const std::string again = files::ReadFile(conf).value_or("");
  CHECK(again.find("[primary]") == again.rfind("[primary]"));
  CHECK(again.find("user = you") != std::string::npos);
  CHECK(again.starts_with("[other]\nx = 1\n"));

  saves::remote::RemoteRequest mirror = request;
  mirror.slot = "mirror";
  mirror.name = "Backup";
  REQUIRE(saves::remote::SaveRemote(config, mirror));
  REQUIRE(saves::remote::ForgetRemote(config, "primary"));
  CHECK_FALSE(saves::remote::FindRemote(config, "primary"));
  const auto kept = saves::remote::FindRemote(config, "mirror");
  REQUIRE(kept);
  CHECK(kept->name == "Backup");
  const std::string left = files::ReadFile(conf).value_or("");
  CHECK(left.find("[mirror]") != std::string::npos);
  CHECK(left.find("[primary]") == std::string::npos);

  CHECK(saves::remote::ForgetRemote(config, "primary"));
  REQUIRE(saves::remote::ForgetRemote(config, "mirror"));
  CHECK(files::ReadFile(conf) == "[other]\nx = 1\n");
}

namespace {

// One computer with a native GOG game saving in <install>/saves, synced with `shared`'s remote.
struct Computer {
  Computer(std::string_view name, const fs::path& shared) : env(name), saves(env.dir / "g/saves") {
    UseFakeRclone(env.config, shared);
    REQUIRE(saves::remote::SaveRemote(env.config, {.slot = "primary",
                                                   .provider = "webdav",
                                                   .name = "nas",
                                                   .folder = "s",
                                                   .values = {{"url", "https://nas.example"}}}));
    game.id = "gog-1";
    game.name = "Game";
    game.source = "gog";
    game.source_ref = "1";
    game.platform = model::Platform::Native;
    game.install_path = (env.dir / "g").string();
    game.overrides = {{"saves.extra_paths", {"<base>/saves"}}};
    fs::create_directories(env.dir / "g");
    device = {.id = std::string(name), .name = std::string(name)};
  }

  Result<nlohmann::json> Run(saves::CloudAction action = saves::CloudAction::Sync) {
    return saves::remote::RunRemote(env.config, env.games.Metadata(), env.games, game, device,
                                    action, [this]() -> Result<void> {
                                      ++snapshots;
                                      if (refuse) return Err("saves_disk_full", "no room");
                                      return {};
                                    });
  }
  std::string Moved(saves::CloudAction action = saves::CloudAction::Sync) {
    const auto run = Run(action);
    if (!run) return run.error().code;
    return run->value("moved", run->value("state", std::string()));
  }
  void Write(const char* file, std::string_view text) { test::Touch(saves / file, text); }
  std::string Read(const char* file) { return files::ReadFile(saves / file).value_or("(none)"); }

  test::TestEnv env;
  fs::path saves;
  model::Game game;
  saves::remote::Device device;
  int snapshots = 0;
  bool refuse = false;
};

}  // namespace

TEST_CASE("saves follow the player between computers, deletions too, and never from an empty one") {
  const fs::path shared = test::TempDir("remote-sync-shared");
  Computer a("remote-sync-a", shared);
  Computer b("remote-sync-b", shared);
  CHECK(a.Moved() == "synced");
  a.Write("one.sav", "1");
  a.Write("two.sav", "2");
  CHECK(a.Moved() == "upload");
  CHECK(a.Moved() == "synced");
  CHECK(b.Moved() == "download");
  CHECK(b.snapshots == 1);
  CHECK(b.Read("one.sav") == "1");
  CHECK(b.Moved() == "synced");

  fs::remove(b.saves / "two.sav");
  b.Write("one.sav", "changed");
  CHECK(b.Moved() == "upload");
  CHECK(a.Moved() == "download");
  CHECK(a.Read("one.sav") == "changed");
  CHECK(a.Read("two.sav") == "(none)");

  // Only the time changed: nothing to send.
  fs::last_write_time(a.saves / "one.sav", fs::file_time_type::clock::now());
  CHECK(a.Moved() == "synced");

  // A prefix made anew: asked about, and neither side loses its saves.
  fs::remove_all(a.saves);
  CHECK(a.Moved() == "saves_conflict");
  CHECK(b.Moved() == "synced");
  CHECK(a.Moved(saves::CloudAction::Upload) == "conflict");
  CHECK(a.Moved(saves::CloudAction::Download) == "download");
  CHECK(a.Read("one.sav") == "changed");

  // The remote emptied: what's here goes up again.
  fs::remove_all(shared / "box");
  CHECK(a.Moved() == "upload");
  CHECK(a.Read("one.sav") == "changed");
}

TEST_CASE("saves changed on both computers wait for a choice, and either choice settles it") {
  const fs::path shared = test::TempDir("remote-conflict-shared");
  Computer a("remote-conflict-a", shared);
  Computer b("remote-conflict-b", shared);
  a.Write("s.sav", "base");
  CHECK(a.Moved() == "upload");
  CHECK(b.Moved() == "download");
  a.Write("s.sav", "from a");
  b.Write("s.sav", "from b!");
  CHECK(a.Moved() == "upload");
  CHECK(b.Moved() == "saves_conflict");
  CHECK(b.Read("s.sav") == "from b!");
  b.refuse = true;
  CHECK(b.Moved(saves::CloudAction::Download) == "saves_disk_full");
  CHECK(b.Read("s.sav") == "from b!");
  b.refuse = false;
  CHECK(b.Moved(saves::CloudAction::Upload) == "upload");
  CHECK(a.Moved() == "download");
  CHECK(a.Read("s.sav") == "from b!");

  // Two uploads at once: two leaves, joined by whichever is chosen.
  const std::string key = saves::remote::KeyFor(a.game, "");
  const auto row = a.env.games.RemoteSyncOf("gog-1", "primary");
  REQUIRE(row);
  const auto info = saves::remote::FindRemote(a.env.config, "primary");
  const saves::remote::Remote remote = saves::remote::RemoteFor(a.env.config, *info);
  a.Write("s.sav", "third");
  CHECK(a.Moved() == "upload");
  const fs::path staged = shared / "fork";
  test::Touch(staged / "files/install/saves/s.sav", "fork");
  const Revision fork{
      .id = "20990101T000000Z-fork",
      .parents = {row->base},
      .device_name = "other",
      .taken_at = 4'000'000'000,
      .files = {{"install", "saves/s.sav", 4, 1,
                 (*saves::remote::Sha256({staged / "files/install/saves/s.sav"}))[0]}}};
  REQUIRE(saves::remote::UploadRevision(a.env.config, remote, key, fork, staged));
  CHECK(a.Moved() == "saves_conflict");
  CHECK(a.Moved(saves::CloudAction::Download) == "download");
  CHECK(a.Read("s.sav") == "fork");
  CHECK(a.Moved() == "synced");
  CHECK(b.Moved() == "download");
  CHECK(b.Read("s.sav") == "fork");
}

TEST_CASE("an upload waits while the remote is offline, and a hostile revision changes nothing") {
  const fs::path shared = test::TempDir("remote-hostile-shared");
  Computer a("remote-hostile-a", shared);
  a.Write("s.sav", "mine");
  CHECK(a.Moved() == "upload");
  a.Write("s.sav", "mine too");
  test::Touch(shared / "offline");
  CHECK(a.Moved() == "remote_offline");
  fs::remove(shared / "offline");
  CHECK(a.Moved() == "upload");

  const std::string key = saves::remote::KeyFor(a.game, "");
  const auto info = saves::remote::FindRemote(a.env.config, "primary");
  const saves::remote::Remote remote = saves::remote::RemoteFor(a.env.config, *info);
  const auto plant = [&](const char* root, const char* path, const std::string& id) {
    const fs::path staged = shared / id;
    test::Touch(staged / "files" / root / path, "evil");
    const std::string base = a.env.games.RemoteSyncOf("gog-1", "primary")->base;
    const Revision evil{.id = id,
                        .parents = {base},
                        .taken_at = 1,
                        .files = {{root, path, 4, 1,
                                   (*saves::remote::Sha256({staged / "files" / root / path}))[0]}}};
    REQUIRE(saves::remote::UploadRevision(a.env.config, remote, key, evil, staged));
  };
  plant("home", ".bashrc", "evil-home");
  CHECK(a.Moved() == "remote_unsafe");
  REQUIRE(saves::remote::DeleteRevision(a.env.config, remote, key, "evil-home"));
  plant("install", "game.sh", "evil-install");
  CHECK(a.Moved() == "remote_unsafe");
  REQUIRE(saves::remote::DeleteRevision(a.env.config, remote, key, "evil-install"));
  plant("prefix_user", "x.sav", "evil-prefix");
  CHECK(a.Moved() == "saves_unplaceable");
  REQUIRE(saves::remote::DeleteRevision(a.env.config, remote, key, "evil-prefix"));
  fs::create_directories(a.env.dir / "outside");
  fs::create_directory_symlink(a.env.dir / "outside", a.saves / "link");
  plant("install", "saves/link/x.sav", "evil-link");
  CHECK(a.Moved() == "remote_unsafe");
  CHECK_FALSE(fs::exists(a.env.dir / "outside/x.sav"));
  REQUIRE(saves::remote::DeleteRevision(a.env.config, remote, key, "evil-link"));
  // A symlink a wildcard would match, as one it doesn't.
  a.game.overrides = {{"saves.extra_paths", {"<base>/**/saves"}}};
  CHECK(a.Moved() == "synced");
  fs::create_directories(a.env.dir / "outside/saves");
  fs::create_directory_symlink(a.env.dir / "outside", a.env.dir / "g/evil");
  plant("install", "evil/saves/x.sav", "evil-wildcard");
  CHECK(a.Moved() == "remote_unsafe");
  CHECK_FALSE(fs::exists(a.env.dir / "outside/saves/x.sav"));
  CHECK(a.snapshots == 0);
  CHECK(a.Read("s.sav") == "mine too");
}

TEST_CASE("a download cut short is finished before anything else") {
  const fs::path shared = test::TempDir("remote-cut-shared");
  Computer a("remote-cut-a", shared);
  Computer b("remote-cut-b", shared);
  a.Write("one.sav", "1");
  a.Write("two.sav", "2");
  CHECK(a.Moved() == "upload");
  CHECK(b.Moved() == "download");
  a.Write("one.sav", "new 1");
  a.Write("two.sav", "new 2");
  CHECK(a.Moved() == "upload");

  // As a crash after the first file leaves it.
  const auto info = saves::remote::FindRemote(b.env.config, "primary");
  const saves::remote::Remote remote = saves::remote::RemoteFor(b.env.config, *info);
  const std::string key = saves::remote::KeyFor(b.game, "");
  const auto leaves =
      saves::remote::Leaves(*saves::remote::ListRevisions(b.env.config, remote, key));
  REQUIRE(leaves.size() == 1);
  const fs::path staging = saves::remote::StagingFolder(b.env.config, b.game);
  fs::create_directories(staging);
  REQUIRE(saves::remote::DownloadRevision(b.env.config, remote, key, leaves[0], staging));
  test::Touch(staging / "applying.json", saves::remote::ManifestJson(leaves[0]).dump());
  b.Write("one.sav", "new 1");

  CHECK(b.Moved() == "synced");
  CHECK(b.snapshots == 2);
  CHECK(b.Read("two.sav") == "new 2");
  CHECK_FALSE(fs::exists(staging));

  // Given up instead, as after playing: the half-replaced saves are compared, never overwritten.
  a.Write("one.sav", "newer 1");
  CHECK(a.Moved() == "upload");
  test::Touch(staging / "applying.json", "not json");
  b.Write("one.sav", "played");
  CHECK(b.Moved() == "saves_conflict");
  CHECK(b.Read("one.sav") == "played");
}

TEST_CASE("a remote connected through the API syncs a game before and after play and is mirrored") {
  const fs::path dir = test::TempDir("remote-api");
  test::LiveServer server(dir);
  auto client = server.Client();
  UseFakeRclone(server.MutableConfig(), dir);
  const auto connect = [&](const char* slot, const char* url) {
    return test::AwaitJob(
        client, client.Put(std::format("/v1/saves/remote/{}", slot),
                           nlohmann::json{{"provider", "webdav"},
                                          {"name", "nas"},
                                          {"values", {{"url", url}, {"password", "hunter22"}}}}
                               .dump(),
                           "application/json"));
  };
  CHECK(connect("primary", "http://nas.example").value("state", "") == "failed");
  // One that can't be reached replaces nothing.
  REQUIRE(connect("primary", "https://nas.example").value("state", "") == "finished");
  test::Touch(dir / "offline");
  CHECK(connect("primary", "https://other.example").value("state", "") == "failed");
  CHECK(files::ReadFile(dir / "rclone.conf")->contains("https://nas.example"));
  fs::remove(dir / "offline");
  REQUIRE(connect("mirror", "https://nas2.example").value("state", "") == "finished");

  model::Game game;
  game.id = "itch-1";
  game.name = "Game";
  game.source = "itch";
  game.source_ref = "1";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = (dir / "g").string();
  game.overrides = {{"saves.extra_paths", {"<base>/saves"}}, {"saves.enabled", false}};
  test::Touch(dir / "g/saves/s.sav", "one");
  REQUIRE(server.games().Upsert(game));

  REQUIRE(server.services().SavesBeforeLaunch(game, ""));
  server.services().saves.WaitIdle();
  const auto kept = [&](const char* remote) {
    std::string found;
    for (const auto& entry : fs::recursive_directory_iterator(dir / remote))
      if (entry.path().filename() == "s.sav")
        found += files::ReadFile(entry.path()).value_or("") + " ";
    return found;
  };
  CHECK(kept("box") == "one ");
  CHECK(kept("mirror") == "one ");

  // Offline after playing: owed, shown, and sent by the next sync.
  test::Touch(dir / "g/saves/s.sav", "two!");
  test::Touch(dir / "offline");
  server.services().SavesAfterPlay("itch-1", [] {});
  server.services().saves.WaitIdle();
  CHECK(test::GetJson(client, "/v1/saves/remote")["pending"][0].value("error", "") ==
        "remote_offline");
  REQUIRE(server.services().SavesBeforeLaunch(game, ""));  // plays with the saves here
  fs::remove(dir / "offline");
  CHECK(test::AwaitJob(client, client.Post("/v1/saves/remote/sync")).value("state", "") ==
        "finished");
  server.services().saves.WaitIdle();
  CHECK(kept("box").contains("two!"));
  CHECK(kept("mirror").contains("two!"));
  const auto shown = test::GetJson(client, "/v1/games/itch-1/saves")["cloud"];
  CHECK(shown.value("kind", "") == "remote");
  CHECK(shown.value("state", "") == "synced");

  // The password is in no reply, no argument and not in the open on disk.
  CHECK_FALSE(test::GetJson(client, "/v1/saves/remote").dump().contains("hunter22"));
  for (const char* file : {"calls", "rclone.conf", "remotes.json"})
    CHECK_FALSE(files::ReadFile(dir / file)->contains("hunter22"));

  REQUIRE(client.Delete("/v1/saves/remote/primary")->status == 200);
  CHECK(test::GetJson(client, "/v1/saves/remote")["mirror"].is_null());
  CHECK(kept("box").contains("two!"));
}
