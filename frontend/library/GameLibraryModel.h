#pragma once

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QString>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {

// The whole library as last heard from mirad, once for the whole window:
// the grid, the source pages and the sidebar all read this one copy through
// their own GameFilterProxy.
//
// Every change is a row insert, update or removal, never a rebuild, so views
// keep their selection and scroll position through it. A row's roles are
// GameTileDelegate's.
class GameLibraryModel : public QAbstractListModel {
  Q_OBJECT

public:
  explicit GameLibraryModel(QObject* parent = nullptr);

  // A fresh GET /v1/games: becomes the library, as updates, inserts and
  // removals, so views keep their selection through a relist.
  void Replace(const std::vector<GameSummary>& games);
  // Records from an event or a reply: updated in place, or appended.
  void Upsert(const std::vector<GameSummary>& games);
  void Remove(const std::vector<std::string>& ids);
  void RemoveSource(const std::string& source);
  // Ahead of mirad's own game.state, e.g. right after a launch it accepted.
  void SetRunning(const std::string& id, bool running);
  // Repaints one game's row: its cover arrived, or its install moved along.
  void Touch(const std::string& id);

  const GameSummary* Find(const std::string& id) const;
  const std::vector<GameSummary>& Games() const { return games_; }
  QModelIndex IndexOf(const std::string& id) const;

  // The status line override for a game mid-install ("Installing… 1.2 GB").
  std::function<QString(const std::string& id)> status_text;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;

signals:
  // Once per burst of changes (a scan, mirad's replay), after the rows moved.
  void Changed();

private:
  void RebuildIndex();
  void NoteChanged();

  std::vector<GameSummary> games_;
  std::unordered_map<std::string, int> rows_;  // id -> row
  bool change_pending_ = false;
};

// One view's slice of the library: a filter key ("all", "hidden", a status,
// ...), a search, optionally one source, and the sidebar's sort.
class GameFilterProxy : public QSortFilterProxyModel {
  Q_OBJECT

public:
  explicit GameFilterProxy(GameLibraryModel* library, QObject* parent = nullptr);

  // Whether `game` belongs under filter `key`. Shared with the filter counts.
  static bool MatchesKey(const GameSummary& game, const QString& key);

  void SetFilterKey(const QString& key);
  void SetSearch(const QString& text);
  // Only this source's games, hidden ones included; empty for every source.
  void SetSource(const std::string& source);
  // The sidebar's sort.
  void SetSort(const std::string& key, bool descending);

  const GameSummary* GameAt(const QModelIndex& index) const;

protected:
  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;
  bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
  // Applies `change` to the filter's inputs and refilters the rows.
  void ChangeFilter(const std::function<void()>& change);

  GameLibraryModel* library_;
  QString key_ = "all";
  QString search_;
  std::string source_;
  std::string sort_key_ = "name";
  bool descending_ = false;};

}  // namespace mira_gui
