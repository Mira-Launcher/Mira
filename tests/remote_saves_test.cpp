#include <doctest.h>

#include <filesystem>
#include <format>

#include "config/Config.h"
#include "core/Files.h"
#include "saves/remote/Rclone.h"
#include "saves/remote/Revisions.h"
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
[ -e '{0}/offline' ] && exit 5
at() {{ case "$1" in box:*) echo "{0}/box/${{1#box:}}" ;; *) echo "$1" ;; esac; }}
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
  const saves::remote::Remote remote{.config_file = dir / "rclone.conf", .root = "box:"};

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
