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
  std::vector<std::pair<QString, QString>> copies;  // (source, ref), in store order
  std::string protondb_tier;   // from the first copy that has one
  std::string review_summary;  // Steam's review label, likewise
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

  // Fetches again; one already running is left to finish.
  void Refresh();
  // Refresh(), unless the last one is recent.
  void RefreshIfStale();
  const std::vector<StoreTitle>& Titles() const { return titles_; }
  // A list arrived at least once.
  bool Listed() const { return fetched_.isValid(); }

signals:
  void Changed();

private:
  std::vector<StoreTitle> titles_;
  QDateTime fetched_;
  bool loading_ = false;
  bool again_ = false;  // asked again while loading
};

}  // namespace mira_gui
