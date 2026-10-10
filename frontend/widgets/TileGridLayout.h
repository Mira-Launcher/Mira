#pragma once

#include <QLayout>
#include <QList>
#include <QWidget>

#include <algorithm>

namespace mira_gui {

// Equal columns, as many as fit `min_width` each; a row is as tall as its tallest item.
class TileGridLayout : public QLayout {
public:
  explicit TileGridLayout(QWidget* parent, int min_width, int gap = 12)
      : QLayout(parent), min_width_(min_width), gap_(gap) {
    setContentsMargins(0, 0, 0, 0);
  }
  ~TileGridLayout() override {
    while (QLayoutItem* item = takeAt(0)) delete item;
  }

  void addItem(QLayoutItem* item) override { items_.append(item); }
  int count() const override { return static_cast<int>(items_.size()); }
  QLayoutItem* itemAt(int index) const override { return items_.value(index); }
  QLayoutItem* takeAt(int index) override {
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
  }
  // Puts the widgets' items in this order; others keep theirs, after them.
  void Reorder(const QList<QWidget*>& widgets) {
    int next = 0;
    for (QWidget* widget : widgets) {
      const int from = indexOf(widget);
      if (from >= 0) items_.move(from, next++);
    }
    invalidate();
  }

  Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return Arrange(QRect(0, 0, width, 0), false); }
  void setGeometry(const QRect& rect) override {
    QLayout::setGeometry(rect);
    Arrange(rect, true);
  }
  QSize sizeHint() const override { return {min_width_, heightForWidth(min_width_)}; }
  QSize minimumSize() const override { return {min_width_, 0}; }

private:
  int Arrange(const QRect& rect, bool apply) const {
    QList<QLayoutItem*> shown;
    for (QLayoutItem* item : items_) {
      if (!item->isEmpty()) shown.append(item);
    }
    const int columns = std::max(1, (rect.width() + gap_) / (min_width_ + gap_));
    const int width = std::max(0, (rect.width() - (columns - 1) * gap_) / columns);
    int y = rect.y();
    for (int first = 0; first < shown.size(); first += columns) {
      const int last = std::min<int>(first + columns, shown.size());
      int height = 0;
      for (int i = first; i < last; ++i) {
        QLayoutItem* item = shown[i];
        height = std::max(height, item->hasHeightForWidth() ? item->heightForWidth(width) : item->sizeHint().height());
      }
      if (apply) {
        for (int i = first; i < last; ++i) {
          shown[i]->setGeometry(QRect(rect.x() + (i - first) * (width + gap_), y, width, height));
        }
      }
      y += height + gap_;
    }
    return shown.isEmpty() ? 0 : y - gap_ - rect.y();
  }

  int min_width_;
  int gap_;
  QList<QLayoutItem*> items_;
};

}  // namespace mira_gui
