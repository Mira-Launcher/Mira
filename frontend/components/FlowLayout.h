#pragma once

#include <QLayout>
#include <QList>
#include <QWidget>
#include <algorithm>

namespace mira_gui {

// Lays its items out left to right, wrapping onto a new line when one won't fit.
class FlowLayout : public QLayout {
 public:
  explicit FlowLayout(QWidget* parent, int gap = 6) : QLayout(parent), gap_(gap) {
    setContentsMargins(0, 0, 0, 0);
  }
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

  // Prefers the width that fits every item on one line, rather than one item's. A layout that sizes
  // from hints for a moment then lays it out on one line, not wrapped one item per line.
  void SetPreferOneLine(bool one_line) {
    one_line_ = one_line;
    invalidate();
  }

  // Spreads each line's items edge to edge, the space between them growing rather than leaving
  // room at the end.
  void SetSpread(bool spread) {
    spread_ = spread;
    invalidate();
  }

  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return Arrange(QRect(0, 0, width, 0), false); }
  void setGeometry(const QRect& rect) override {
    QLayout::setGeometry(rect);
    Arrange(rect, true);
  }
  QSize sizeHint() const override {
    if (!one_line_) return minimumSize();
    QSize size;
    for (const QLayoutItem* item : items_) {
      if (item->widget() != nullptr && item->widget()->isHidden()) continue;
      const QSize hint = item->sizeHint();
      size = QSize(size.width() + (size.width() > 0 ? gap_ : 0) + hint.width(),
                   std::max(size.height(), hint.height()));
    }
    return size.expandedTo(minimumSize());
  }
  QSize minimumSize() const override {
    QSize size;
    for (const QLayoutItem* item : items_) size = size.expandedTo(item->minimumSize());
    return size;
  }

 private:
  int Arrange(const QRect& rect, bool apply) const {
    // Split into lines first, so a spread line knows the room it has left.
    QList<QList<QLayoutItem*>> lines(1);
    int x = rect.x();
    for (QLayoutItem* item : items_) {
      if (item->widget() != nullptr && item->widget()->isHidden()) continue;
      const int width = item->sizeHint().width();
      if (x > rect.x() && x + width > rect.right() + 1) {
        lines.append(QList<QLayoutItem*>());
        x = rect.x();
      }
      lines.last().append(item);
      x += width + gap_;
    }
    int y = rect.y();
    int spread_gap = gap_;
    for (qsizetype i = 0; i < lines.size(); ++i) {
      const QList<QLayoutItem*>& line = lines[i];
      int used = 0;
      int line_height = 0;
      for (const QLayoutItem* item : line) {
        used += item->sizeHint().width();
        line_height = std::max(line_height, item->sizeHint().height());
      }
      // The last of several lines keeps the gap of the full ones above, so columns stay put.
      const bool last = i == lines.size() - 1 && lines.size() > 1;
      if (spread_ && !last && line.size() > 1)
        spread_gap = (rect.width() - used) / static_cast<int>(line.size() - 1);
      const int line_gap = spread_ ? spread_gap : gap_;
      x = rect.x();
      for (QLayoutItem* item : line) {
        const QSize hint = item->sizeHint();
        if (apply) item->setGeometry(QRect(QPoint(x, y), hint));
        x += hint.width() + line_gap;
      }
      if (i > 0 || !line.isEmpty()) y += line_height + (i + 1 < lines.size() ? gap_ : 0);
    }
    return y - rect.y();
  }

  int gap_;
  bool one_line_ = false;
  bool spread_ = false;
  QList<QLayoutItem*> items_;
};

}  // namespace mira_gui
