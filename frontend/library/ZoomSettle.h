#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <functional>

class QTimer;

namespace mira_gui {

class ArtworkStore;

// The covers of one view while its zoom moves: scaled quickly at each step and not kept, then,
// once it stops, `settled` runs to draw them smooth and every other width the view kept goes,
// unless another view draws at it (each holds its width in the ArtworkStore).
class ZoomSettle : public QObject {
 public:
  ZoomSettle(QObject* parent, ArtworkStore* artwork, int width, std::function<void()> settled);
  // A view closed mid-zoom ends the quick scaling it started, which is the whole app's.
  ~ZoomSettle() override;

  // The view's tiles are now `width` wide.
  void Step(int width);
  // Covers were fetched ahead at `width`, a size the zoom was heading to.
  void Prefetched(int width) { widths_.insert(width); }

 private:
  QPointer<ArtworkStore> artwork_;  // may go first when the window closes
  std::function<void()> settled_;
  QTimer* timer_;
  int width_;
  bool quick_ = false;  // this view's zoom turned quick scaling on
  QSet<int> widths_;    // widths covers may be kept at besides width_
};

}  // namespace mira_gui
