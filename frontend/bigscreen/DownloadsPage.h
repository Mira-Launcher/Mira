#pragma once

#include <QRectF>

#include <utility>
#include <vector>

#include "Page.h"

namespace mira_gui::bigscreen {

// The running install large at the top over its art, the others below it with
// pause, resume and cancel, then the owned games that aren't installed as a
// cover grid to install from.
class DownloadsPage : public Page {
  Q_OBJECT

public:
  explicit DownloadsPage(BigScreenWindow* window);

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  struct Layout {
    // Each item's box, before scrolling, in the order of items_.
    std::vector<QRectF> boxes;
    // Section headings: baseline and text.
    std::vector<std::pair<double, QString>> headings;
  };

  void Refresh();
  void FocusChanged();
  Layout Arrange() const;
  void PaintInstall(QPainter& painter, const QRectF& box, const Item& item, bool hero, bool focused) const;

  // Installs first, then `ready` games to install.
  std::vector<Item> items_;
  int installs_ = 0;
  int focus_ = 0;
};

}  // namespace mira_gui::bigscreen
