#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <utility>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {

// One game you own but haven't installed, as a search shows it: one entry
// even when several stores sell it, with each store's copy.
struct OwnedMatch {
  QString title;
  std::vector<std::pair<QString, QString>> copies;      // (source, ref), in store order
  std::vector<std::pair<QString, QString>> store_urls;  // (source, page), for copies that have one
  std::string protondb_tier;                            // from the first copy that has one
  std::string review_summary;                           // Steam's review label, likewise
  int review_percent = -1;
};

// The not-installed titles whose name contains `query` (as the library search
// matches), one entry per game across stores, by name. Empty for an empty query.
std::vector<OwnedMatch> MatchOwned(const std::vector<StoreTitle>& titles, const QString& query);
// The same, in the stores' own order, and every title for an empty query.
std::vector<OwnedMatch> GroupOwned(const std::vector<StoreTitle>& titles, const QString& query);

// Every store's owned titles, fetched in the background so a search and the
// Not installed tab never wait on a store. Small (one entry per title),
// replaced on each refresh, and fetched again when mirad finds a store's list changed.
class OwnedTitles : public QObject {
  Q_OBJECT

 public:
  explicit OwnedTitles(QObject* parent = nullptr);

  // Fetches again; one already running is left to finish. `fresh` waits for every store's own
  // answer rather than the lists mirad stored, for a refresh the user asked for.
  void Refresh(bool fresh = false);
  // Refresh(), unless the last one is recent.
  void RefreshIfStale();
  const std::vector<StoreTitle>& Titles() const { return titles_; }
  // A list arrived at least once.
  bool Listed() const { return fetched_.isValid(); }
  // The last fetch failed.
  bool Failed() const { return failed_; }

 signals:
  void Changed();

 private:
  std::vector<StoreTitle> titles_;
  QDateTime fetched_;
  bool loading_ = false;
  bool failed_ = false;
  bool again_ = false;  // asked again while loading
  bool again_fresh_ = false;
};

}  // namespace mira_gui
