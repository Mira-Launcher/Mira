#include "OwnedTitles.h"

#include <algorithm>
#include <map>
#include <set>

#include "../client/EventHub.h"
#include "../client/Events.h"
#include "../client/api/Stores.h"

namespace mira_gui {
namespace {

constexpr qint64 kStaleSeconds = 10 * 60;

// One game's name across stores: "The Outer Worlds: Spacer’s Choice Edition"
// on GOG and "...Spacer's Choice Edition" on Epic are the same game.
QString SameGameKey(const QString& title) {
  QString key;
  for (const QChar c : title.toLower()) {
    if (c.isLetterOrNumber()) key += c;
  }
  return key;
}

}  // namespace

std::vector<OwnedMatch> MatchOwned(const std::vector<StoreTitle>& titles, const QString& query) {
  if (query.trimmed().isEmpty()) return {};
  std::vector<OwnedMatch> matches = GroupOwned(titles, query);
  std::ranges::sort(matches, [](const OwnedMatch& a, const OwnedMatch& b) {
    return QString::localeAwareCompare(a.title, b.title) < 0;
  });
  return matches;
}

std::vector<OwnedMatch> GroupOwned(const std::vector<StoreTitle>& titles, const QString& query) {
  const QString needle = query.trimmed();
  std::vector<OwnedMatch> matches;
  std::map<QString, std::size_t> by_key;
  std::set<QString> installed;  // a game installed from any store is already in the library
  for (const StoreTitle& title : titles) {
    const QString name = QString::fromStdString(title.title);
    if (!title.owned || !name.contains(needle, Qt::CaseInsensitive)) continue;
    const QString key = SameGameKey(name);
    if (title.installed) {
      installed.insert(key);
      continue;
    }
    auto [found, added] = by_key.try_emplace(key, matches.size());
    if (added) matches.emplace_back().title = name;
    OwnedMatch& match = matches[found->second];
    match.copies.emplace_back(QString::fromStdString(title.source), QString::fromStdString(title.ref));
    // The same game's ratings, from whichever copy has them.
    if (match.protondb_tier.empty()) match.protondb_tier = title.protondb_tier;
    if (match.review_summary.empty()) match.review_summary = title.review_summary;
    if (match.review_percent < 0) match.review_percent = title.review_percent;
  }
  std::erase_if(matches, [&](const OwnedMatch& match) { return installed.contains(SameGameKey(match.title)); });
  return matches;
}

OwnedTitles::OwnedTitles(QObject* parent) : QObject(parent) {
  // A store's list changed behind the stored one mirad answered with.
  connect(EventHub::Instance(), &EventHub::Received, this, [this](const std::string& type, const std::string& data, bool live) {
    if (CatalogCheckEvent check; live && fetched_.isValid() && events::ParseCatalogCheck(type, data, &check) && check.changed) {
      Refresh();
    }
  });
}

void OwnedTitles::Refresh() {
  if (loading_) {
    again_ = true;  // what's in flight may predate the change
    return;
  }
  loading_ = true;
  api::GetStoreLibraryAsync(this, std::string(), false, [this](StoreLibraryResult result) {
    loading_ = false;
    if (std::exchange(again_, false)) Refresh();
    if (!result.ok) return;  // the last list stays; a store that's down isn't worth a notice here
    titles_ = std::move(result.titles);
    fetched_ = QDateTime::currentDateTimeUtc();
    emit Changed();
  });
}

void OwnedTitles::RefreshIfStale() {
  if (!fetched_.isValid() || fetched_.secsTo(QDateTime::currentDateTimeUtc()) > kStaleSeconds) Refresh();
}

}  // namespace mira_gui
