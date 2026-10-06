#include "OwnedTitles.h"

#include <algorithm>
#include <map>
#include <set>

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
  const QString needle = query.trimmed();
  std::vector<OwnedMatch> matches;
  if (needle.isEmpty()) return matches;
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
    if (added) matches.push_back({.title = name, .copies = {}});
    matches[found->second].copies.emplace_back(QString::fromStdString(title.source), QString::fromStdString(title.ref));
  }
  std::erase_if(matches, [&](const OwnedMatch& match) { return installed.contains(SameGameKey(match.title)); });
  std::ranges::sort(matches, [](const OwnedMatch& a, const OwnedMatch& b) {
    return QString::localeAwareCompare(a.title, b.title) < 0;
  });
  return matches;
}

OwnedTitles::OwnedTitles(QObject* parent) : QObject(parent) {}

void OwnedTitles::Refresh() {
  if (loading_) return;
  loading_ = true;
  api::GetStoreLibraryAsync(this, std::string(), [this](StoreLibraryResult result) {
    loading_ = false;
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
