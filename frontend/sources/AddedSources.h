#pragma once

#include <QString>
#include <QWidget>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Sources.h"

class QLabel;

namespace mira_gui {

class SourceTile;
class Switch;
class TileGridLayout;

struct SourceEntry {
  SourceInfo source;
  bool ready = false;  // set up
  bool enabled = true;
  int games = 0;
  bool in_sidebar = true;
  std::string account;           // signed-in account, if the source says
  std::int64_t imported_at = 0;  // unix seconds; 0 if never
};

// The sources that are set up, as tiles in sidebar order, games and apps
// apart. A tile opens its source; its switches keep its games in the library
// and its row in the sidebar; ⋯ imports or removes it.
class AddedSources : public QWidget {
  Q_OBJECT

public:
  explicit AddedSources(QWidget* parent = nullptr);

  // The set-up entries, in sidebar order. Tiles are kept across calls, so an
  // import in progress keeps its line.
  void SetEntries(const std::vector<SourceEntry>& entries);
  // Shows only the tiles whose name or kind contains `text`.
  void SetFilter(const QString& text);

signals:
  void OpenRequested(QString id);
  void SettingsRequested(QString id);
  void SidebarToggled(QString id, bool shown);
  void EnabledToggled(QString id, bool enabled);
  void Imported(QString id);  // an import finished and changed something
  void Removed(QString id);

private:
  struct Tile {
    SourceEntry entry;
    SourceTile* tile = nullptr;
    Switch* in_library = nullptr;  // none for Local, which is always in
    Switch* in_sidebar = nullptr;
    QWidget* more = nullptr;
    bool importing = false;
    QString note;  // an import's result, shown instead of the status from then on
  };

  Tile& Build(const SourceEntry& entry);
  void Update(Tile& tile);
  void ShowMenu(const QString& id);
  void Import(const QString& id);
  void ApplyFilter();

  QLabel* games_heading_ = nullptr;
  QWidget* games_grid_ = nullptr;
  QLabel* apps_heading_ = nullptr;
  QWidget* apps_grid_ = nullptr;
  QLabel* empty_ = nullptr;
  std::map<QString, Tile> tiles_;  // by source id
  QString filter_;
};

}  // namespace mira_gui
