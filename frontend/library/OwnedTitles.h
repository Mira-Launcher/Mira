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
};

// The not-installed titles whose name contains `query` (as the library search
// matches), one entry per game across stores, by name. Empty for an empty query.
std::vector<OwnedMatch> MatchOwned(const std::vector<StoreTitle>& titles, const QString& query);

// Every store's owned titles, fetched in the background so a search never
// waits on a store. Small (one entry per title) and replaced on each refresh.
class OwnedTitles : public QObject {
  Q_OBJECT

public:
  explicit OwnedTitles(QObject* parent = nullptr);

  // Fetches again; one already running is left to finish.
  void Refresh();
  // Refresh(), unless the last one is recent.
  void RefreshIfStale();
  const std::vector<StoreTitle>& Titles() const { return titles_; }

signals:
  void Changed();

private:
  std::vector<StoreTitle> titles_;
  QDateTime fetched_;
  bool loading_ = false;
};

}  // namespace mira_gui
