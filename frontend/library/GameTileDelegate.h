#pragma once

#include <QSize>
#include <QStyledItemDelegate>

class QAbstractItemView;

namespace mira_gui {

class ArtworkStore;

// Paints one cover tile in the library grid: the artwork, a scrim, the title
// over it, and the status.
//
// Everything it draws comes from the index's roles, plus the cover: an
// index's own DecorationRole if it has one (a store title), else the
// artwork store's cover for its IdRole at this delegate's tile size.
class GameTileDelegate : public QStyledItemDelegate {
public:
  // The roles the grid sets on each item and this delegate reads.
  enum Role {
    IdRole = Qt::UserRole + 1,
    NameRole,
    StatusRole,
    RunningRole,
    // Optional: a button-like pill painted in the tile's top-right corner
    // ("Install", "Installing…"). Hit-test clicks with ActionRect.
    ActionRole,
    ActionEnabledRole,
    // Optional: replaces the status line's text ("Installing… 1.2 GB").
    StatusTextRole,
    // Pinned by the user: a pin badge in the tile's top-right corner.
    PinnedRole,
    // Optional: the game's source id, drawn as a small colored mark.
    SourceRole,
    // Optional: 0..1, or below 0 while busy with no percentage, drawn as a
    // rail under the status line.
    ProgressRole,
    // mirad wants its pick of executable checked: "Not checked" on the status line.
    NeedsCheckRole,
  };

  // Where the ActionRole pill sits inside a tile's cell.
  static QRect ActionRect(const QRect& cell, const QString& text, const QFont& font);

  GameTileDelegate(QObject* parent, QSize tile, ArtworkStore* artwork = nullptr);

  void SetTileSize(QSize tile);
  void SetShowStatus(bool show) { show_status_ = show; }
  void SetShowSourceMark(bool show) { show_source_mark_ = show; }
  void SetShowPinBadge(bool show) { show_pin_badge_ = show; }
  QSize TileSize() const { return tile_; }
  // Shows `text` over game `id`'s tile in `view` for a few seconds, e.g. why a double-click did nothing.
  static void ShowNote(QAbstractItemView* view, const QString& id, const QString& text);

  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override;

private:
  QSize tile_;
  bool show_status_ = true;
  bool show_source_mark_ = true;
  bool show_pin_badge_ = true;
  ArtworkStore* artwork_;
  QString note_id_;  // the tile ShowNote is drawing on, if any
  QString note_;
};

}  // namespace mira_gui
