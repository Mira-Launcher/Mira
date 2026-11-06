#include <doctest.h>

#include <QTimeZone>

#include "game/SavesText.h"

using namespace mira_gui;

namespace {

// 21 Oct 2026, 22:00 UTC.
const QDateTime kNow(QDate(2026, 10, 21), QTime(22, 0), QTimeZone::UTC);

std::int64_t At(int days_ago, int hour, int minute) {
  return kNow.addDays(-days_ago).date().startOfDay(QTimeZone::UTC).toSecsSinceEpoch() +
         hour * 3600 + minute * 60;
}

SaveSnapshot Backup(int days_ago, const std::string& reason) {
  return {.id = "b", .taken_at = At(days_ago, 21, 14), .reason = reason, .files = 1, .bytes = 1};
}

CloudSaves Cloud(const std::string& store, const std::string& state,
                 const std::string& reason = {}) {
  CloudSaves cloud;
  cloud.store = store;
  cloud.state = state;
  cloud.reason = reason;
  return cloud;
}

}  // namespace

TEST_CASE("A backup's time reads as today, yesterday, then its date") {
  CHECK(saves_text::When(At(0, 21, 14), kNow) == "Today, 21:14");
  CHECK(saves_text::When(At(1, 23, 2), kNow) == "Yesterday, 23:02");
  // After other words, as in "Restored the backup from today, 21:14".
  CHECK(saves_text::When(At(0, 21, 14), kNow, true) == "today, 21:14");
  CHECK(saves_text::When(At(1, 23, 2), kNow, true) == "yesterday, 23:02");
  CHECK(saves_text::When(At(5, 9, 5), kNow).endsWith(", 09:05"));
  CHECK_FALSE(saves_text::When(At(5, 9, 5), kNow).contains("2026"));
  CHECK(saves_text::When(At(400, 9, 5), kNow).contains("2025"));
}

TEST_CASE("The card's saves line counts backups and says when the last was taken") {
  SavesResult saves;
  saves.status = "kept";
  saves.snapshots = {Backup(0, "after_play"), Backup(3, "after_play")};
  QString line = saves_text::Summary(saves, kNow);
  CHECK(line.contains("2 backups"));
  CHECK(line.contains("today"));
  saves.snapshots = {Backup(1, "after_play")};
  line = saves_text::Summary(saves, kNow);
  CHECK(line.contains("1 backup"));
  CHECK(line.contains("yesterday"));
  saves.snapshots = {Backup(0, "manual")};
  CHECK(saves_text::Summary(saves, kNow).contains("21:14"));  // a manual one says when

  // A conflict or a failed backup is what the line says, and it reads as a warning.
  CHECK_FALSE(saves_text::NeedsAttention(saves));
  const QString kept = saves_text::Summary(saves, kNow);
  saves.cloud = Cloud("Epic Games", "conflict");
  const QString conflict = saves_text::Summary(saves, kNow);
  CHECK(conflict.contains("Epic Games"));
  CHECK(saves_text::NeedsAttention(saves));
  saves.cloud.reset();
  saves.last_error = ApiError("too big", "saves_too_large");
  const QString failed = saves_text::Summary(saves, kNow);
  CHECK(saves_text::NeedsAttention(saves));
  CHECK(failed != kept);
  CHECK(failed != conflict);

  // Without backups, what stands in for them.
  SavesResult none;
  none.status = "no_files";
  const QString no_files = saves_text::Summary(none, kNow);
  none.status = "steam_cloud";
  CHECK(saves_text::Summary(none, kNow).contains("Steam Cloud"));
  CHECK_FALSE(no_files.contains("Steam Cloud"));
}

TEST_CASE("A save folder reads from the prefix's user folder or the home folder") {
  CHECK(saves_text::Path("/home/u/Games/prefixes/well/drive_c/users/steamuser/AppData/LocalLow/"
                         "Billy Basso/Animal Well",
                         "/home/u") == "prefix › AppData/LocalLow/Billy Basso/Animal Well");
  CHECK(saves_text::Path("/home/u/.config/unity3d/Game", "/home/u") == "~/.config/unity3d/Game");
  CHECK(saves_text::Path("/home/user2/save", "/home/u") == "/home/user2/save");
  CHECK(saves_text::Path("/mnt/games/fir/Saved", "/home/u") == "/mnt/games/fir/Saved");
}

TEST_CASE("The cloud's state names its store, and an unsupported one says why") {
  const QString synced = saves_text::CloudState(Cloud("GOG", "synced"));
  const QString unknown = saves_text::CloudState(Cloud("GOG", "unknown"));
  CHECK(synced.contains("GOG"));
  CHECK(unknown.contains("GOG"));
  CHECK(synced != unknown);
  CHECK(saves_text::CloudState(Cloud("GOG", "unsupported", "GOG keeps none for native games")) ==
        "GOG keeps none for native games");
}
