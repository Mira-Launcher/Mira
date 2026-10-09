#include "GameLibraryModel.h"

#include <QDateTime>
#include <QTimer>

#include <algorithm>

#include "../sources/Sources.h"
#include "GamePresentation.h"
#include "GameTileDelegate.h"
#include "LibrarySort.h"

namespace mira_gui {

GameLibraryModel::GameLibraryModel(QObject* parent) : QAbstractListModel(parent) {
  launch_tick_ = new QTimer(this);
  launch_tick_->setInterval(400);
  connect(launch_tick_, &QTimer::timeout, this, &GameLibraryModel::TickLaunching);
}

void GameLibraryModel::Replace(const std::vector<GameSummary>& games) {
  std::unordered_map<std::string, bool> listed;
  for (const GameSummary& game : games) listed[game.id] = true;
  std::vector<std::string> gone;
  for (const GameSummary& game : games_) {
    if (!listed.contains(game.id)) gone.push_back(game.id);
  }
  Remove(gone);
  Upsert(games);
}

void GameLibraryModel::Upsert(const std::vector<GameSummary>& games) {
  std::vector<const GameSummary*> added;
  std::unordered_map<std::string, std::size_t> added_at;  // an id repeated in one batch keeps its last copy
  for (const GameSummary& game : games) {
    const auto found = rows_.find(game.id);
    if (found == rows_.end()) {
      const auto [slot, inserted] = added_at.try_emplace(game.id, added.size());
      if (inserted) {
        added.push_back(&game);
      } else {
        added[slot->second] = &game;
      }
      continue;
    }
    if (games_[found->second] == game) continue;
    games_[found->second] = game;
    emit dataChanged(index(found->second, 0), index(found->second, 0));
  }
  if (!added.empty()) {
    const int first = static_cast<int>(games_.size());
    beginInsertRows(QModelIndex(), first, first + static_cast<int>(added.size()) - 1);
    for (const GameSummary* game : added) {
      rows_[game->id] = static_cast<int>(games_.size());
      games_.push_back(*game);
    }
    endInsertRows();
  }
  NoteChanged();
}

void GameLibraryModel::Remove(const std::vector<std::string>& ids) {
  std::vector<int> rows;
  for (const std::string& id : ids) {
    if (const auto found = rows_.find(id); found != rows_.end()) rows.push_back(found->second);
  }
  if (rows.empty()) return;
  std::ranges::sort(rows, std::greater{});
  // Each run of rows costs an index rebuild and a re-sort in every proxy, so rows scattered
  // across the library (a store's games removed) go in one reset instead.
  int runs = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i == 0 || rows[i] != rows[i - 1] - 1) ++runs;
  }
  if (rows.size() == games_.size() || runs > kMaxRemovedRuns) {
    beginResetModel();
    std::vector<bool> dropped(games_.size(), false);
    for (const int row : rows) dropped[row] = true;
    std::size_t kept = 0;
    for (std::size_t row = 0; row < games_.size(); ++row) {
      if (!dropped[row]) games_[kept++] = std::move(games_[row]);
    }
    games_.resize(kept);
    RebuildIndex();
    endResetModel();
    NoteChanged();
    return;
  }
  // Highest first, one begin/end per contiguous run, with rows_ already true when the signal fires.
  for (std::size_t i = 0; i < rows.size();) {
    const int last = rows[i];
    int first = last;
    for (++i; i < rows.size() && rows[i] == first - 1; ++i) --first;
    beginRemoveRows(QModelIndex(), first, last);
    games_.erase(games_.begin() + first, games_.begin() + last + 1);
    RebuildIndex();
    endRemoveRows();
  }
  NoteChanged();
}

void GameLibraryModel::RemoveSource(const std::string& source) {
  std::vector<std::string> ids;
  for (const GameSummary& game : games_) {
    if (game.source == source) ids.push_back(game.id);
  }
  Remove(ids);
}

void GameLibraryModel::SetLaunching(const std::string& id, bool launching) {
  if (launching) {
    launching_[id] = QDateTime::currentMSecsSinceEpoch();
    launch_tick_->start();
  } else if (launching_.erase(id) == 0) {
    return;
  }
  Touch(id);
}

void GameLibraryModel::TickLaunching() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  std::vector<std::string> ids;
  for (const auto& [id, since] : launching_) ids.push_back(id);
  for (const std::string& id : ids) {
    if (now - launching_[id] >= kLaunchingMs) launching_.erase(id);
    Touch(id);
  }
  if (launching_.empty()) launch_tick_->stop();
}

void GameLibraryModel::SetRunning(const std::string& id, bool running) {
  if (!running) SetLaunching(id, false);
  const auto found = rows_.find(id);
  if (found == rows_.end() || games_[found->second].running == running) return;
  games_[found->second].running = running;
  emit dataChanged(index(found->second, 0), index(found->second, 0));
  NoteChanged();
}

void GameLibraryModel::Touch(const std::string& id) {
  if (const auto found = rows_.find(id); found != rows_.end()) {
    emit dataChanged(index(found->second, 0), index(found->second, 0));
  }
}

const GameSummary* GameLibraryModel::Find(const std::string& id) const {
  const auto found = rows_.find(id);
  return found != rows_.end() ? &games_[found->second] : nullptr;
}

std::vector<const GameSummary*> GameLibraryModel::RecentlyPlayed(int count) const {
  std::vector<const GameSummary*> running;
  std::vector<const GameSummary*> played;
  for (const GameSummary& game : games_) {
    if (game.running) {  // anything open shows, apps included
      running.push_back(&game);
    } else if (!IsApp(game) && game.last_played_at && !IsHidden(game)) {
      played.push_back(&game);
    }
  }
  std::ranges::sort(played, [](const GameSummary* a, const GameSummary* b) {
    return *a->last_played_at > *b->last_played_at;
  });
  played.resize(std::min(
      played.size(), static_cast<size_t>(std::max(0, count - static_cast<int>(running.size())))));
  played.insert(played.begin(), running.begin(), running.end());
  return played;
}

QModelIndex GameLibraryModel::IndexOf(const std::string& id) const {
  const auto found = rows_.find(id);
  return found != rows_.end() ? index(found->second, 0) : QModelIndex();
}

int GameLibraryModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(games_.size());
}

QVariant GameLibraryModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= static_cast<int>(games_.size())) return {};
  const GameSummary& game = games_[index.row()];
  switch (role) {
    case GameTileDelegate::IdRole: return QString::fromStdString(game.id);
    case Qt::DisplayRole:
    case GameTileDelegate::NameRole: return QString::fromStdString(game.name);
    case GameTileDelegate::StatusRole: return QString::fromStdString(game.status);
    case GameTileDelegate::RunningRole: return game.running;
    case GameTileDelegate::StatusTextRole:
    case GameTileDelegate::ProgressRole:
    case GameTileDelegate::ProgressDetailRole: {
      const auto installing = install_progress ? install_progress(game.id) : std::nullopt;
      if (!installing) {
        const auto launching = launching_.find(game.id);
        if (role != GameTileDelegate::StatusTextRole || launching == launching_.end()) return {};
        const qint64 dots = (QDateTime::currentMSecsSinceEpoch() - launching->second) / 400 % 4;
        return "Launching" + QString(".").repeated(static_cast<int>(dots));
      }
      if (role == GameTileDelegate::StatusTextRole) return installing->status;
      if (role == GameTileDelegate::ProgressDetailRole) return installing->detail;
      return installing->fraction;
    }
    case GameTileDelegate::SourceRole: return QString::fromStdString(game.source);
    case GameTileDelegate::NeedsCheckRole: return game.needs_check;
    case GameTileDelegate::AppRole: return IsApp(game);
    default: return {};
  }
}

void GameLibraryModel::RebuildIndex() {
  rows_.clear();
  for (int row = 0; row < static_cast<int>(games_.size()); ++row) rows_[games_[row].id] = row;
}

void GameLibraryModel::NoteChanged() {
  if (change_pending_) return;
  change_pending_ = true;
  QTimer::singleShot(0, this, [this] {
    change_pending_ = false;
    emit Changed();
  });
}

GameFilterProxy::GameFilterProxy(GameLibraryModel* library, QObject* parent)
    : QSortFilterProxyModel(parent), library_(library) {
  setSourceModel(library);
  // Re-sorted and re-filtered as rows change, moving them rather than resetting.
  setDynamicSortFilter(true);
  QSortFilterProxyModel::sort(0, Qt::AscendingOrder);
}

bool GameFilterProxy::MatchesKey(const GameSummary& game, const QString& key, bool apps_in_all) {
  // Store launchers (Battle.net, ...) live on their source pages, not here.
  if (game.source == "launcher") return false;
  if (key == "hidden") return IsHidden(game);
  // Every other filter excludes a hidden game: "not displayed by default"
  // means not in "All games" either, not just off the initial screen.
  if (IsHidden(game)) return false;
  if (key == "all") return apps_in_all || !IsApp(game);
  if (key == "running") return game.running;
  if (key == "games") return !IsApp(game);
  if (key == "apps") return IsApp(game);
  if (key == "never") return !game.last_played_at.has_value() && !IsApp(game);
  if (key == "attention") {
    return game.status == "needs_install" || game.status == "broken" || game.status == "missing" || game.needs_check;
  }
  return key == QLatin1StringView(game.status.data(), static_cast<qsizetype>(game.status.size()));
}

namespace {


bool AnyTagStartsWith(const GameSummary& game, const QString& prefix) {
  return std::ranges::any_of(game.tags, [&](const std::string& tag) {
    return !IsMeaningTag(tag) &&
           QString::fromStdString(tag).startsWith(prefix, Qt::CaseInsensitive);
  });
}

}  // namespace

std::vector<std::string> TagOrder(const std::vector<GameSummary>& games) {
  struct Tally {
    std::string name;
    int count = 0;
  };
  std::vector<std::string> folders;
  std::vector<Tally> others;
  const auto known = [](const std::vector<std::string>& list, const std::string& tag) {
    return std::ranges::any_of(list, [&](const std::string& t) { return SameTag(t, tag); });
  };
  for (const GameSummary& game : games) {
    for (const std::string& folder : game.folder_tags.value_or(std::vector<std::string>())) {
      if (!known(folders, folder)) folders.push_back(folder);
    }
  }
  for (const GameSummary& game : games) {
    for (const std::string& tag : game.tags) {
      if (IsMeaningTag(tag) || known(folders, tag)) continue;
      auto found = std::ranges::find_if(others, [&](const Tally& t) { return SameTag(t.name, tag); });
      if (found == others.end()) {
        others.push_back({tag, 0});
        found = others.end() - 1;
      }
      ++found->count;
    }
  }
  // As GET /v1/tags ranks them, so the Tags page agrees.
  std::ranges::stable_sort(others, [](const Tally& a, const Tally& b) {
    return a.count != b.count ? a.count > b.count : a.name < b.name;
  });
  for (const Tally& tag : others) folders.push_back(tag.name);
  return folders;
}

std::vector<LibraryTag> TagsUnder(const std::vector<GameSummary>& games, const QString& key, bool apps_in_all) {
  const auto is_folder = [&](const std::string& tag) {
    return std::ranges::any_of(games, [&](const GameSummary& game) {
      return std::ranges::any_of(game.folder_tags.value_or(std::vector<std::string>()),
                                 [&](const std::string& f) { return SameTag(f, tag); });
    });
  };
  std::vector<LibraryTag> tags;
  for (const std::string& tag : TagOrder(games)) tags.push_back({QString::fromStdString(tag), 0, is_folder(tag)});
  for (const GameSummary& game : games) {
    if (!GameFilterProxy::MatchesKey(game, key, apps_in_all)) continue;
    for (LibraryTag& tag : tags) {
      if (std::ranges::any_of(game.tags, [&](const std::string& t) { return SameTag(t, tag.tag.toStdString()); }))
        ++tag.count;
    }
  }
  // A tag no game under the filter has is left out, unless it's a folder tag.
  std::erase_if(tags, [](const LibraryTag& t) { return t.count == 0 && !t.folder; });
  return tags;
}

bool GameFilterProxy::MatchesSearch(const GameSummary& game, const QString& search) {
  const QString name = QString::fromStdString(game.name);
  const QStringList words = search.split(QChar(' '), Qt::SkipEmptyParts);
  return std::ranges::all_of(words, [&](const QString& word) {
    if (word.startsWith(QChar('#'))) return AnyTagStartsWith(game, word.mid(1));
    return name.contains(word, Qt::CaseInsensitive) || AnyTagStartsWith(game, word);
  });
}

bool GameFilterProxy::HasTags(const GameSummary& game, const QStringList& tags) {
  return std::ranges::all_of(tags, [&](const QString& wanted) {
    return std::ranges::any_of(game.tags, [&](const std::string& tag) {
      return QString::fromStdString(tag).compare(wanted, Qt::CaseInsensitive) == 0;
    });
  });
}

void GameFilterProxy::SetTags(const QStringList& tags) {
  if (tags == tags_) return;
  ChangeFilter([&] { tags_ = tags; });
}

void GameFilterProxy::ChangeFilter(const std::function<void()>& change) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  beginFilterChange();
  change();
  endFilterChange(Direction::Rows);
#else
  change();
  invalidateFilter();
#endif
}

void GameFilterProxy::SetFilterKey(const QString& key) {
  if (key == key_) return;
  ChangeFilter([&] { key_ = key; });
}

void GameFilterProxy::SetSearch(const QString& text) {
  const QString trimmed = text.trimmed();
  if (trimmed == search_) return;
  ChangeFilter([&] { search_ = trimmed; });
}

void GameFilterProxy::SetAppsInAll(bool apps_in_all) {
  if (apps_in_all == apps_in_all_) return;
  ChangeFilter([&] { apps_in_all_ = apps_in_all; });
}

void GameFilterProxy::SetSource(const std::string& source) {
  ChangeFilter([&] { source_ = source; });
}

void GameFilterProxy::SetSort(const std::string& key, bool descending) {
  if (key == sort_key_ && descending == descending_) return;
  sort_key_ = key;
  descending_ = descending;
  invalidate();
}

const GameSummary* GameFilterProxy::GameAt(const QModelIndex& index) const {
  if (!index.isValid()) return nullptr;
  const QModelIndex source = mapToSource(index);
  return source.isValid() && source.row() < static_cast<int>(library_->Games().size())
             ? &library_->Games()[source.row()]
             : nullptr;
}

bool GameFilterProxy::filterAcceptsRow(int source_row, const QModelIndex&) const {
  const GameSummary& game = library_->Games()[source_row];
  if (!search_.isEmpty() && !MatchesSearch(game, search_)) return false;
  if (!HasTags(game, tags_)) return false;
  if (!source_.empty()) return SourceIdOf(game.source) == source_;
  return MatchesKey(game, key_, apps_in_all_);
}

bool GameFilterProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const {
  const auto& games = library_->Games();
  return GameLess(games[left.row()], games[right.row()], sort_key_, descending_);
}

}  // namespace mira_gui
