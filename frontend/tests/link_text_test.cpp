#include <doctest.h>

#include "activity/DownloadTracker.h"
#include "downloads/LinkText.h"

using namespace mira_gui;

TEST_CASE("pasted links keep their order, without repeats or other words") {
  const auto links = downloads::ParseLinks(
      "Part one: https://a.example/x.rar\nhttps://a.example/y.rar  magnet:?xt=abc\n"
      "https://a.example/x.rar\nftp://nope");
  CHECK(links == std::vector<std::string>{"https://a.example/x.rar", "https://a.example/y.rar",
                                          "magnet:?xt=abc"});
}

TEST_CASE("a game's name comes from its first file") {
  CHECK(downloads::GuessName("Some_Game.part01.rar") == "Some Game");
  CHECK(downloads::GuessName("Some.Game.7z.001") == "Some Game");
  CHECK(downloads::GuessName("Mr. Game v1.2.zip") == "Mr. Game v1.2");
}

TEST_CASE("a link download's row follows its state") {
  DownloadInfo download;
  download.id = "abc";
  download.name = "Game";
  download.state = "downloading";
  download.total = 100;
  download.done = 25;
  download.speed = 5;

  DownloadTracker tracker;
  tracker.SetLink(download);
  const auto* entry = tracker.Find(DownloadTracker::KeyFor(DownloadTracker::Kind::Link, {}, "abc"));
  REQUIRE(entry != nullptr);
  CHECK(tracker.NameFor(*entry) == "Game");
  CHECK(entry->state == DownloadTracker::State::Running);
  CHECK(entry->progress == doctest::Approx(0.25));
  CHECK(tracker.RunningCount() == 1);

  download.state = "password";
  tracker.SetLink(download);
  CHECK(tracker.Entries().size() == 1);
  CHECK(tracker.Entries().front().state == DownloadTracker::State::Paused);
  CHECK(tracker.Entries().front().message == "Waiting for a password");

  tracker.RemoveLink("abc");
  CHECK(tracker.Entries().empty());
}

TEST_CASE("A download a service is fetching shows the service and its progress") {
  DownloadInfo download;
  download.id = "a";
  download.state = "service";
  download.service = "TorBox";
  download.service_progress = 0.25;
  const auto entry = DownloadTracker::FromLink(download);
  CHECK(entry.message == "TorBox is getting it…");
  CHECK(entry.progress == 0.25);
}
