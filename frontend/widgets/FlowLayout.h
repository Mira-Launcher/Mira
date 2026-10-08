#pragma once

#include <QLayout>
#include <QList>
#include <QWidget>

#include <algorithm>

namespace mira_gui {

// Lays its items out left to right, wrapping onto a new line when one won't fit.
class FlowLayout : public QLayout {
public:
  explicit FlowLayout(QWidget* parent, int gap = 6) : QLayout(parent), gap_(gap) { setContentsMargins(0, 0, 0, 0); }
  ~FlowLayout() override {
    while (QLayoutItem* item = takeAt(0)) delete item;
  }

  void addItem(QLayoutItem* item) override { items_.append(item); }
  int count() const override { return static_cast<int>(items_.size()); }
  QLayoutItem* itemAt(int index) const override { return items_.value(index); }
  QLayoutItem* takeAt(int index) override {
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
  }
  // Moves the item for `widget` to `index`, e.g. to keep a trailing button last.
  void Move(QWidget* widget, int index) {
    const int from = indexOf(widget);
    if (from >= 0) items_.move(from, std::clamp(index, 0, count() - 1));
    invalidate();
  }

  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return Arrange(QRect(0, 0, width, 0), false); }
  void setGeometry(const QRect& rect) override {
    QLayout::setGeometry(rect);
    Arrange(rect, true);
  }
  QSize sizeHint() const override { return minimumSize(); }
  QSize minimumSize() const override {
    QSize size;
    for (const QLayoutItem* item : items_) size = size.expandedTo(item->minimumSize());
    return size;
  }

private:
  int Arrange(const QRect& rect, bool apply) const {
    int x = rect.x();
    int y = rect.y();
    int line_height = 0;
    for (QLayoutItem* item : items_) {
      if (item->widget() != nullptr && item->widget()->isHidden()) continue;
      const QSize hint = item->sizeHint();
      if (x > rect.x() && x + hint.width() > rect.right() + 1) {
        x = rect.x();
        y += line_height + gap_;
        line_height = 0;
      }
      line_height = std::max(line_height, hint.height());
      if (apply) item->setGeometry(QRect(QPoint(x, y), hint));
      x += hint.width() + gap_;
    }
    return y + line_height - rect.y();
  }

  int gap_;
  QList<QLayoutItem*> items_;
};

}  // namespace mira_gui
