#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../client/ApiError.h"

class QTimer;

namespace mira_gui {

// Everything mirad is installing or downloading, built from the event
// stream: game installers, store installs and updates,
// launcher installs, store helper downloads and runner downloads. mirad
// replays its recent events to a new stream, so this also knows about work
// started before the GUI was.
//
// It also lists mirad's jobs (scans, imports, moves, deletes), from job.*.
//
// Only game installers report how far along they are (bytes written);
// the rest are only started, finished or failed.
class DownloadTracker : public QObject {
  Q_OBJECT

public:
  enum class Kind { Game, Title, Launcher, Tool, Runner, Job };
  enum class State { Running, Paused, Finished, Failed };

  struct Entry {
    QString key;  // see KeyFor
    Kind kind = Kind::Game;
    QString source;  // a source id; empty for Game and Runner; a Job's kind
    QString ref;     // game id, title ref, bundle key, launcher id, runner tag or job id
    bool update = false;
    State state = State::Running;
    qint64 bytes = 0;  // Game only
    double progress = -1;  // 0..1 when the source reports it (store installs, jobs with steps)
    qint64 eta_seconds = -1;
    double bytes_per_second = -1;
    QString message;       // a Job's current step
    ApiError error;
    QDateTime changed;
  };

  explicit DownloadTracker(QObject* parent = nullptr);

  static QString KeyFor(Kind kind, const QString& source, const QString& ref);
  // The live log of the work an entry stands for (GET /v1/logs/<channel>); work with no log of its own reads the daemon's.
  static QString LogChannelFor(const Entry& entry);
  // "42% · 12.5 MB/s · 3 min left", as much as the source reported; empty
  // when it reported nothing. `short_form` leaves out the speed.
  static QString ProgressText(const Entry& entry, bool short_form = false);

  // What a cover shows for a running install: the rail's fraction (negative while unknown), the
  // status line ("42% · 3 min left", or what it's doing) and a quieter line under it (the speed).
  struct TileProgress {
    double fraction = -1;
    QString status;
    QString detail;
  };
  static TileProgress TileProgressFor(const Entry& entry);

  // Epic, GOG and Amazon titles: their installs can be paused.
  static bool CanPause(const Entry& entry);

  // Returns whether the event was one of ours.
  bool HandleEvent(const std::string& type, const std::string& data);
  // After a reconnect: ends running jobs whose finish event was missed.
  void RecheckJobs();
  // Rows for the installs mirad has paused, e.g. after reconnecting.
  void LoadPaused();

  // Newest first.
  const std::vector<Entry>& Entries() const { return entries_; }
  const Entry* Find(const QString& key) const;
  int RunningCount() const;
  void ClearFinished();

  // A display name for an entry. Store titles use NoteTitle's names, then
  // the game they became, then their ref.
  QString NameFor(const Entry& entry) const;
  void NoteTitle(const QString& source, const QString& ref, const QString& title);

  // The installed game an entry is, or became; empty if none.
  static QString GameIdFor(const Entry& entry);
  // The mirad job a running entry's work runs as, for cancelling it; empty if none is known.
  QString JobFor(const Entry& entry) const;

  // Set by the owner: a tracked game's name, or empty.
  std::function<QString(const std::string& id)> game_name;
  // Set by the owner: a source's display name.
  std::function<QString(const QString& source)> source_name;

signals:
  // One entry was added or changed; empty when several were (ClearFinished).
  void Changed(const QString& key);

private:
  Entry& Upsert(Kind kind, const QString& source, const QString& ref);
  // A cancelled entry's row goes away rather than reading as a failure. True if it was one.
  bool DropIfCancelled(const Entry& entry);
  bool HandleJobEvent(const std::string& type, const std::string& data);
  void Poll();
  void ResolveNames(const QString& source);

  std::vector<Entry> entries_;
  QHash<QString, QString> titles_;  // "<source>:<ref>" -> title
  QSet<QString> asked_sources_;     // title lists already fetched to name entries
  std::map<QString, QString> job_by_target_;  // running jobs with their own rows: target -> job id
  QTimer* poll_ = nullptr;
};

}  // namespace mira_gui
