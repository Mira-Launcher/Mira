#pragma once

#include <QObject>
#include <QSet>

#include <functional>

class QTimer;

namespace mira_gui {

class ArtworkStore;

// The covers of one view while its zoom moves: scaled quickly at each step and not kept, then,
// once it stops, `settled` runs to draw them smooth and every other width the view kept goes.
class ZoomSettle : public QObject {
public:
  ZoomSettle(QObject* parent, ArtworkStore* artwork, int width, std::function<void()> settled);

  // The view's tiles are now `width` wide.
  void Step(int width);
  // Covers were fetched ahead at `width`, a size the zoom was heading to.
  void Prefetched(int width) { widths_.insert(width); }

private:
  ArtworkStore* artwork_;
  std::function<void()> settled_;
  QTimer* timer_;
  int width_;
  QSet<int> widths_;  // widths covers may be kept at besides width_
};

}  // namespace mira_gui
