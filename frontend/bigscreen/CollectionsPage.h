#pragma once

#include <vector>

#include "Page.h"

namespace mira_gui::bigscreen {

// The user's tags as cards of their games' covers, and a tag's games as a cover grid.
class CollectionsPage : public Page {
  Q_OBJECT

public:
  explicit CollectionsPage(BigScreenWindow* window);

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  struct Collection {
    QString tag;
    std::vector<Item> items;
  };
  void Rebuild();
  const Collection& Current() const;
  int Columns() const;
  void DrawCard(QPainter& painter, const QRectF& box, const Collection& collection, bool focused) const;

  std::vector<Collection> collections_;
  bool open_ = false;  // showing one tag's games
  int collection_ = 0;
  int game_ = 0;
};

}  // namespace mira_gui::bigscreen
