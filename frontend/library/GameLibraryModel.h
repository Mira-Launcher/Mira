#pragma once

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../activity/DownloadTracker.h"
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
  // "Launching" on its tile from the click until its window has had time to show, or until it stops.
  void SetLaunching(const std::string& id, bool launching);
  // Repaints one game's row: its cover arrived, or its install moved along.
  void Touch(const std::string& id);

  const GameSummary* Find(const std::string& id) const;
  const std::vector<GameSummary>& Games() const { return games_; }
  // What recently played lists at `count`: running games first, then the last
  // played, `count` in all, more only when more than `count` run. Hidden games
  // only while they run, so they can still be stopped. Apps only while they run:
  // they are played like any game, but a closed one doesn't take a place here.
  std::vector<const GameSummary*> RecentlyPlayed(int count) const;
  QModelIndex IndexOf(const std::string& id) const;

  // A game mid-install: its rail, status line and detail line; nullopt otherwise.
  std::function<std::optional<DownloadTracker::TileProgress>(const std::string& id)> install_progress;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;

signals:
  // Once per burst of changes (a scan, mirad's replay), after the rows moved.
  void Changed();

private:
  void RebuildIndex();
  void NoteChanged();

  void TickLaunching();

  // How long a launch shows as launching: about as long as a Windows app takes to open its window.
  static constexpr qint64 kLaunchingMs = 8000;
  // Past this many separate runs of removed rows, Remove resets the model instead.
  static constexpr int kMaxRemovedRuns = 16;

  std::vector<GameSummary> games_;
  std::unordered_map<std::string, int> rows_;  // id -> row
  bool change_pending_ = false;
  std::unordered_map<std::string, qint64> launching_;  // id -> when it was launched, in ms
  QTimer* launch_tick_ = nullptr;  // animates the launching tiles' dots
};

// One view's slice of the library: a filter key ("all", "hidden", a status,
// ...), a search, optionally one source, and the sidebar's sort.
class GameFilterProxy : public QSortFilterProxyModel {
  Q_OBJECT

public:
  explicit GameFilterProxy(GameLibraryModel* library, QObject* parent = nullptr);

  // Whether `game` belongs under filter `key`. Shared with the filter counts.
  // `apps_in_all`: whether All lists apps too, not only games.
  static bool MatchesKey(const GameSummary& game, const QString& key, bool apps_in_all = true);

  void SetFilterKey(const QString& key);
  void SetSearch(const QString& text);
  void SetAppsInAll(bool apps_in_all);
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
  bool apps_in_all_ = true;
  std::string source_;
  std::string sort_key_ = "name";
  bool descending_ = false;
};

}  // namespace mira_gui
