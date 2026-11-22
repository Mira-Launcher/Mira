#pragma once

#include <QList>
#include <QWidget>

namespace mira_gui {

// The rows of a card or an opened expander, with a line between each two shown. Drawn here rather
// than by the card so the lines scroll with the rows. `rows` is the owner's list, in order.
class DividedRows : public QWidget {
 public:
  DividedRows(const QList<QWidget*>* rows, QWidget* parent);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  const QList<QWidget*>* rows_;
};

// The rows under a scrolling card's rows, with a line over each shown, the first included.
class DividedFooter : public QWidget {
 public:
  using QWidget::QWidget;

 protected:
  void paintEvent(QPaintEvent* event) override;
};

}  // namespace mira_gui
