#include <doctest.h>

#include "activity/DownloadTracker.h"

using namespace mira_gui;
using State = DownloadTracker::State;

namespace {

State StateOf(const DownloadTracker& tracker, const QString& key) {
  const DownloadTracker::Entry* entry = tracker.Find(key);
  REQUIRE(entry != nullptr);
  return entry->state;
}

}  // namespace

TEST_CASE("DownloadTracker follows a store install from start to finish") {
  DownloadTracker tracker;
  // Named up front, so it doesn't ask mirad for the store's titles.
  tracker.NoteTitle("gog", "1207658924", "Alan Wake");

  CHECK(tracker.HandleEvent("library.install.started", R"({"source": "gog", "ref": "1207658924", "update": false})"));
  CHECK(tracker.RunningCount() == 1);
  const DownloadTracker::Entry* entry = tracker.Find("gog:1207658924");
  REQUIRE(entry != nullptr);
  CHECK(tracker.NameFor(*entry).toStdString() == "Alan Wake");
  CHECK(DownloadTracker::GameIdFor(*entry).toStdString() == "gog-1207658924");

  tracker.HandleEvent("library.install.failed",
                      R"({"source": "gog", "ref": "1207658924", "update": false, "error": "not signed in",
                          "code": "not_authenticated", "hint": "Sign in to GOG.",
                          "fix": {"kind": "source", "target": "gog", "step": "login"}})");
  CHECK(StateOf(tracker, "gog:1207658924") == State::Failed);
  const ApiError& error = tracker.Find("gog:1207658924")->error;
  CHECK(error.message == "not signed in");
  CHECK(error.hint == "Sign in to GOG.");
  CHECK(error.fix.kind == "source");
  CHECK(error.fix.target == "gog");
  CHECK(tracker.RunningCount() == 0);
}

TEST_CASE("DownloadTracker shows a store download's progress and follows a game's own installer") {
  DownloadTracker tracker;
  tracker.HandleEvent(
      "library.install.progress",
      R"({"source": "gog", "ref": "1", "progress": 0.425, "eta": 3725, "bps": 1048576})");
  const DownloadTracker::Entry* download = tracker.Find("gog:1");
  REQUIRE(download != nullptr);
  CHECK(download->state == State::Running);
  CHECK(download->progress == doctest::Approx(0.425));
  // Time left rounds up to whole minutes, and past an hour reads in hours.
  const QString full = DownloadTracker::ProgressText(*download, /*short_form=*/false);
  CHECK(full.contains("43%"));
  CHECK(full.contains("1 h 3 min left"));
  CHECK(full.contains("/s"));
  CHECK_FALSE(DownloadTracker::ProgressText(*download, /*short_form=*/true).contains("/s"));
  // A cover shows the percentage and time left over the speed.
  const DownloadTracker::TileProgress tile = DownloadTracker::TileProgressFor(*download);
  CHECK(tile.fraction == doctest::Approx(0.425));
  CHECK(tile.status == DownloadTracker::ProgressText(*download, /*short_form=*/true));
  CHECK(tile.detail.endsWith("/s"));
  tracker.HandleEvent("library.install.progress",
                      R"({"source": "gog", "ref": "1", "progress": 0.9, "eta": 30})");
  CHECK(DownloadTracker::ProgressText(*tracker.Find("gog:1"), true).contains("1 min left"));

  CHECK(tracker.HandleEvent("game.install.started", R"({"id": "celeste"})"));
  const QString game_key =
      DownloadTracker::KeyFor(DownloadTracker::Kind::Game, QString(), "celeste");
  CHECK(StateOf(tracker, game_key) == State::Running);
  // An installer with no percentage: a busy rail, what it's doing, and nothing under it yet.
  const DownloadTracker::TileProgress installing = DownloadTracker::TileProgressFor(*tracker.Find(game_key));
  CHECK(installing.fraction < 0);
  CHECK(installing.status == "Installing…");
  CHECK(installing.detail.isEmpty());
  tracker.HandleEvent("game.install.failed", R"({"id": "celeste", "error": "the installer quit",
                                                 "fix": {"kind": "game", "target": "celeste", "step": "install"}})");
  CHECK(StateOf(tracker, game_key) == State::Failed);
  CHECK(tracker.Find(game_key)->error.fix.step == "install");
  CHECK(tracker.RunningCount() == 1);
}

TEST_CASE("DownloadTracker keeps the newest activity first and tells kinds apart") {
  DownloadTracker tracker;
  tracker.source_name = [](const QString& source) { return source == "battlenet" ? "Battle.net" : source; };

  tracker.HandleEvent("launcher.install.started", R"({"id": "battlenet"})");
  tracker.HandleEvent("umu.setup.started", R"({})");
  tracker.HandleEvent("runners.download.started", R"({"kind": "proton", "tag": "GE-Proton10-4"})");
  CHECK_FALSE(tracker.HandleEvent("game.added", R"({"id": "x"})"));

  REQUIRE(tracker.Entries().size() == 3);
  CHECK(tracker.Entries()[0].kind == DownloadTracker::Kind::Runner);
  CHECK(tracker.NameFor(tracker.Entries()[0]).toStdString() == "GE-Proton10-4");
  CHECK(tracker.Entries()[2].kind == DownloadTracker::Kind::Launcher);
  CHECK(tracker.NameFor(tracker.Entries()[2]).toStdString() == "Battle.net");
  CHECK(tracker.NameFor(*tracker.Find("tool:umu")).toStdString() == "umu-launcher");

  // Moves back to the top when it changes.
  tracker.HandleEvent("launcher.install.finished", R"({"id": "battlenet"})");
  CHECK(tracker.Entries()[0].key.toStdString() == "launcher:battlenet");
  CHECK(tracker.RunningCount() == 2);

  tracker.ClearFinished();
  CHECK(tracker.Entries().size() == 2);
  CHECK(tracker.Find("launcher:battlenet") == nullptr);
}

TEST_CASE("DownloadTracker shows a store install once, under the game's name") {
  DownloadTracker tracker;
  tracker.NoteTitle("gog", "1207658924", "Alan Wake");
  tracker.HandleEvent("job.started", R"({"id": "install-1", "kind": "install", "target": "gog-1207658924",
                                         "label": "Installing 1207658924"})");
  tracker.HandleEvent("library.install.started", R"({"source": "gog", "ref": "1207658924", "update": false})");
  REQUIRE(tracker.Entries().size() == 1);
  CHECK(tracker.NameFor(tracker.Entries()[0]).toStdString() == "Alan Wake");

  // A store's own tool setup has no other row, so its job stays.
  tracker.HandleEvent("job.started", R"({"id": "setup-1", "kind": "setup", "target": "gog", "label": "Setting up gogdl"})");
  CHECK(tracker.Entries().size() == 2);
}

TEST_CASE("DownloadTracker knows the job behind a running install, and a cancelled one leaves no row") {
  DownloadTracker tracker;
  tracker.HandleEvent("library.install.started", R"({"source": "epic", "ref": "Fortnite", "update": false})");
  const DownloadTracker::Entry* entry = tracker.Find("epic:Fortnite");
  REQUIRE(entry != nullptr);
  CHECK(tracker.JobFor(*entry).isEmpty());
  tracker.HandleEvent("job.started", R"({"id": "install-7", "kind": "install", "target": "epic-Fortnite", "label": "x"})");
  CHECK(tracker.JobFor(*tracker.Find("epic:Fortnite")).toStdString() == "install-7");

  tracker.HandleEvent("library.install.failed",
                      R"({"source": "epic", "ref": "Fortnite", "update": false, "error": "Cancelled", "code": "cancelled"})");
  tracker.HandleEvent("job.failed", R"({"id": "install-7", "kind": "install", "target": "epic-Fortnite",
                                        "error": {"code": "cancelled", "message": "Cancelled"}})");
  CHECK(tracker.Find("epic:Fortnite") == nullptr);
  CHECK(tracker.RunningCount() == 0);
}

TEST_CASE("DownloadTracker shows a runner download once, with its progress") {
  DownloadTracker tracker;
  tracker.HandleEvent("job.started", R"({"id": "runner-1", "kind": "runner", "target": "proton:GE-Proton10-4",
                                         "label": "Downloading GE-Proton10-4"})");
  tracker.HandleEvent("runners.download.started", R"({"kind": "proton", "tag": "GE-Proton10-4", "name": "GE-Proton10-4"})");
  tracker.HandleEvent("runners.download.progress",
                      R"({"kind": "proton", "tag": "GE-Proton10-4", "name": "GE-Proton10-4", "progress": 0.39})");
  REQUIRE(tracker.Entries().size() == 1);
  CHECK(tracker.Entries()[0].state == State::Running);
  CHECK(tracker.Entries()[0].progress == doctest::Approx(0.39));
  CHECK(DownloadTracker::ProgressText(tracker.Entries()[0]).contains("39%"));

  tracker.HandleEvent("runners.download.finished", R"({"kind": "proton", "tag": "GE-Proton10-4", "name": "GE-Proton10-4"})");
  CHECK(tracker.Entries()[0].state == State::Finished);
  CHECK(tracker.RunningCount() == 0);
}

TEST_CASE("DownloadTracker follows a job by its label and steps") {
  DownloadTracker tracker;
  CHECK(tracker.HandleEvent("job.started",
                            R"({"id": "delete-1", "kind": "delete", "target": "", "label": "Removing 4 games"})"));
  const QString key = DownloadTracker::KeyFor(DownloadTracker::Kind::Job, QString(), "delete-1");
  REQUIRE(tracker.Find(key) != nullptr);
  CHECK(tracker.NameFor(*tracker.Find(key)).toStdString() == "Removing 4 games");

  tracker.HandleEvent("job.progress", R"({"id": "delete-1", "done": 1, "total": 4, "message": "Celeste"})");
  CHECK(tracker.Find(key)->progress == doctest::Approx(0.25));
  CHECK(tracker.Find(key)->message.toStdString() == "Celeste");

  tracker.HandleEvent("job.failed", R"({"id": "delete-1", "kind": "delete", "error": {"code": "io", "message": "busy"}})");
  CHECK(StateOf(tracker, key) == State::Failed);
  CHECK(tracker.Find(key)->error.message == "busy");

  // Nothing to name a job whose start was never seen.
  tracker.HandleEvent("job.finished", R"({"id": "scan-9", "kind": "scan", "result": {}})");
  CHECK(tracker.Find(DownloadTracker::KeyFor(DownloadTracker::Kind::Job, QString(), "scan-9")) == nullptr);
}

TEST_CASE("DownloadTracker keeps finished jobs with steps and drops routine ones") {
  DownloadTracker tracker;
  tracker.HandleEvent("job.started", R"({"id": "scan-1", "kind": "scan", "label": "Scanning your library"})");
  tracker.HandleEvent("job.started", R"({"id": "move-1", "kind": "relocate", "label": "Moving 2 games"})");
  tracker.HandleEvent("job.progress", R"({"id": "move-1", "done": 1, "total": 2})");
  tracker.HandleEvent("job.finished", R"({"id": "scan-1", "kind": "scan", "result": {}})");
  tracker.HandleEvent("job.finished", R"({"id": "move-1", "kind": "relocate", "result": {}})");

  REQUIRE(tracker.Entries().size() == 1);
  CHECK(tracker.Entries()[0].ref.toStdString() == "move-1");
  CHECK(tracker.Entries()[0].state == State::Finished);
}

TEST_CASE("DownloadTracker keeps a paused install's progress and lets it be paused") {
  DownloadTracker tracker;
  tracker.HandleEvent("library.install.progress",
                      R"({"source": "epic", "ref": "abc", "progress": 0.4})");
  tracker.HandleEvent("library.install.paused", R"({"source": "epic", "ref": "abc", "update": false})");
  const DownloadTracker::Entry* entry = tracker.Find("epic:abc");
  REQUIRE(entry != nullptr);
  CHECK(entry->state == State::Paused);
  CHECK(entry->progress == doctest::Approx(0.4));
  CHECK(tracker.RunningCount() == 0);
  CHECK(DownloadTracker::CanPause(*entry));
}
