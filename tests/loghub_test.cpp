#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "core/LogHub.h"

using namespace mira;

TEST_CASE("Each channel keeps its own lines, read from a cursor") {
  loghub::Begin("test:a");
  loghub::Begin("test:b");
  loghub::Append("test:a", "one\ntwo\npart");
  loghub::Append("test:b", "other\n");

  const loghub::Page first = loghub::Read("test:a", nullptr, 10);
  CHECK(first.lines == std::vector<std::string>{"one", "two"});  // "part" waits for its newline
  CHECK(first.active);
  CHECK(loghub::Read("test:b", nullptr, 10).lines == std::vector<std::string>{"other"});

  loghub::Append("test:a", "ial\n");
  const loghub::Page next = loghub::Read("test:a", &first.next, 10);
  CHECK(next.lines == std::vector<std::string>{"partial"});

  loghub::End("test:a");
  CHECK_FALSE(loghub::Read("test:a", &next.next, 10).active);
  CHECK(loghub::Read("test:a", nullptr, 1).lines == std::vector<std::string>{"partial"});
}

TEST_CASE("Starting a channel over drops its old lines") {
  loghub::Begin("test:c");
  loghub::Append("test:c", "old\n");
  const loghub::Page before = loghub::Read("test:c", nullptr, 10);
  loghub::Begin("test:c");
  loghub::Append("test:c", "new\n");
  CHECK(loghub::Read("test:c", &before.next, 10).lines == std::vector<std::string>{"new"});
}

TEST_CASE("A line redrawn with \\r is one live line, then one final line") {
  loghub::Begin("test:d");
  loghub::Append("test:d", "Progress: 10%\rProgress: 20%\rProgress: 30%");
  loghub::Page page = loghub::Read("test:d", nullptr, 10);
  CHECK(page.lines.empty());
  CHECK(page.live == "Progress: 30%");

  loghub::Append("test:d", "\rProgress: 100%\nDone\n");
  page = loghub::Read("test:d", nullptr, 10);
  CHECK(page.lines == std::vector<std::string>{"Progress: 100%", "Done"});
  CHECK(page.live.empty());

  // "\r\n" line endings are not a redraw.
  loghub::Begin("test:e");
  loghub::Append("test:e", "one\r\ntwo\r\n");
  CHECK(loghub::Read("test:e", nullptr, 10).lines == std::vector<std::string>{"one", "two"});
}

TEST_CASE("Repeated download progress lines are one live line, kept once something else follows") {
  loghub::Begin("test:f");
  loghub::Append("test:f", "Downloading corefonts\n");
  loghub::Append("test:f", "  918750K .......... .......... .......... .......... .......... 98% 39.6M 1s\n");
  loghub::Append("test:f", "  918800K .......... .......... .......... .......... .......... 99% 26.8M 1s\n");
  loghub::Page page = loghub::Read("test:f", nullptr, 10);
  CHECK(page.lines == std::vector<std::string>{"Downloading corefonts"});
  CHECK(page.live.find("99%") != std::string::npos);

  loghub::Append("test:f", "Executing cabextract\n");
  page = loghub::Read("test:f", nullptr, 10);
  CHECK(page.lines.size() == 3);
  CHECK(page.lines[1].find("99%") != std::string::npos);  // the last reading, not every one
  CHECK(page.live.empty());
}

TEST_CASE("A channel is journaled to disk as it is written, and read back from there after a restart") {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "mira-loghub-journal-test";
  std::filesystem::remove_all(dir);
  loghub::SetJournalDirectory(dir);

  loghub::Begin("setup:journal-test");
  loghub::Append("setup:journal-test", "first\nsecond\n");
  // On disk already, before End: this is what a crash leaves behind.
  std::ifstream on_disk(dir / "setup_journal-test.log");
  std::string line;
  std::getline(on_disk, line);
  CHECK(line == "first");
  std::getline(on_disk, line);
  CHECK(line == "second");

  // Channels nothing has written to since mirad started are read from their file.
  {
    std::ofstream(dir / "setup_earlier-run.log") << "from\nbefore\n";
    const loghub::Page page = loghub::Read("setup:earlier-run", nullptr, 10);
    CHECK(page.lines == std::vector<std::string>{"from", "before"});
    CHECK_FALSE(page.active);
  }

  // Game and daemon logs have files of their own.
  loghub::Begin("game:journal-test");
  CHECK_FALSE(std::filesystem::exists(dir / "game_journal-test.log"));

  loghub::End("setup:journal-test");
  loghub::SetJournalDirectory({});
  std::filesystem::remove_all(dir);
}
