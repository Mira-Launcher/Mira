#include "GameLibraryModel.h"

#include <QTimer>

#include <algorithm>

#include "GamePresentation.h"
#include "GameTileDelegate.h"
#include "LibrarySort.h"

namespace mira_gui {

GameLibraryModel::GameLibraryModel(QObject* parent) : QAbstractListModel(parent) {}

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
  if (rows.size() == games_.size()) {
    beginResetModel();
    games_.clear();
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

void GameLibraryModel::SetRunning(const std::string& id, bool running) {
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
    case GameTileDelegate::PinnedRole: return IsPinned(game);
    case GameTileDelegate::StatusTextRole:
    case GameTileDelegate::ProgressRole:
    case GameTileDelegate::ProgressDetailRole: {
      const auto installing = install_progress ? install_progress(game.id) : std::nullopt;
      if (!installing) return {};
      if (role == GameTileDelegate::StatusTextRole) return installing->status;
      if (role == GameTileDelegate::ProgressDetailRole) return installing->detail;
      return installing->fraction;
    }
    case GameTileDelegate::SourceRole: return QString::fromStdString(game.source);
    case GameTileDelegate::NeedsCheckRole: return game.needs_check;
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

bool GameFilterProxy::MatchesKey(const GameSummary& game, const QString& key) {
  // Store launchers (Battle.net, ...) live on their source pages, not here.
  if (game.source == "launcher") return false;
  if (key == "hidden") return IsHidden(game);
  // Every other filter excludes a hidden game: "not displayed by default"
  // means not in "All games" either, not just off the initial screen.
  if (IsHidden(game)) return false;
  if (key == "all") return true;
  if (key == "running") return game.running;
  if (key == "games") return !IsApp(game);
  if (key == "apps") return IsApp(game);
  if (key == "never") return !game.last_played_at.has_value() && !IsApp(game);
  if (key == "attention") {
    return game.status == "needs_install" || game.status == "broken" || game.status == "missing" || game.needs_check;
  }
  return key == QLatin1StringView(game.status.data(), static_cast<qsizetype>(game.status.size()));
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
  if (!search_.isEmpty() && !QString::fromStdString(game.name).contains(search_, Qt::CaseInsensitive)) return false;
  if (!source_.empty()) return game.source == source_;
  return MatchesKey(game, key_);
}

bool GameFilterProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const {
  const auto& games = library_->Games();
  return GameLess(games[left.row()], games[right.row()], sort_key_, descending_);
}

}  // namespace mira_gui
