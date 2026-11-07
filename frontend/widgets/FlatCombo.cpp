#include "FlatCombo.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QScrollBar>
#include <QStyleOptionComboBox>
#include <QStyledItemDelegate>

namespace mira_gui {

FlatCombo::FlatCombo(QWidget* parent) : QComboBox(parent) {
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  // Qt's own popup items measure without their text and elide it when drawn; these measure what
  // they draw, so the popup can fit the longest (see showPopup).
  setItemDelegate(new QStyledItemDelegate(this));
  connect(this, &QComboBox::currentIndexChanged, this, [this] { updateGeometry(); });
  // An item renamed in place keeps the index, so its text's width comes from the model.
  connect(model(), &QAbstractItemModel::dataChanged, this, [this] { updateGeometry(); });
}

void FlatCombo::changeEvent(QEvent* event) {
  QComboBox::changeEvent(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) updateGeometry();
}

QSize FlatCombo::sizeHint() const {
  // The style sheet's sizeFromContents ignores the text for a combo, so measure what the box,
  // its padding and its arrow take from a sample width, and add the text to that.
  QStyleOptionComboBox option;
  initStyleOption(&option);
  constexpr int kSample = 400;
  option.rect = QRect(0, 0, kSample, QComboBox::sizeHint().height());
  const QRect field =
      style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
  int text = fontMetrics().horizontalAdvance(currentText()) + 2;  // the rounding Qt elides on
  if (!itemIcon(currentIndex()).isNull()) text += iconSize().width() + 4;
  return {kSample - field.width() + text, option.rect.height()};
}

QSize FlatCombo::minimumSizeHint() const {
  return sizeHint();
}

void FlatCombo::showPopup() {
  // Qt widens the popup past the box only in some styles; under the style sheet it stays the
  // box's width and every longer option shows as "…".
  QAbstractItemView* list = view();
  int width = list->sizeHintForColumn(0) + 2 * list->frameWidth();
  if (count() > maxVisibleItems()) width += list->verticalScrollBar()->sizeHint().width();
  list->setMinimumWidth(width);
  QComboBox::showPopup();
}

}  // namespace mira_gui
