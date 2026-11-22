#include "ChangeBar.h"

#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

namespace mira_gui {

ChangeBar::ChangeBar(QWidget* over) : QFrame(over) {
  setObjectName("change_bar");
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(16, 8, 8, 8);
  layout->setSpacing(10);
  text_ = new QLabel(this);
  layout->addWidget(text_);
  layout->addSpacing(6);
  discard_ = new QPushButton("Discard", this);
  connect(discard_, &QPushButton::clicked, this, &ChangeBar::DiscardClicked);
  layout->addWidget(discard_);
  save_ = new QPushButton("Save", this);
  save_->setDefault(true);
  connect(save_, &QPushButton::clicked, this, &ChangeBar::SaveClicked);
  layout->addWidget(save_);

  auto* shadow = new QGraphicsDropShadowEffect(this);
  shadow->setBlurRadius(28);
  shadow->setOffset(0, 8);
  shadow->setColor(QColor(0, 0, 0, 90));
  setGraphicsEffect(shadow);

  over->installEventFilter(this);
  hide();
}

void ChangeBar::SetCount(int count) {
  SetText(count <= 0   ? QString()
          : count == 1 ? QString("1 unsaved change")
                       : QString("%1 unsaved changes").arg(count));
}

void ChangeBar::SetText(const QString& text, const QString& save, const QString& discard) {
  text_->setText(text);
  save_->setText(save);
  discard_->setText(discard);
  setVisible(!text.isEmpty());
  if (!text.isEmpty()) {
    adjustSize();
    Place();
    raise();
  }
  emit RoomChanged(text.isEmpty() ? 0 : RoomNeeded());
}

int ChangeBar::RoomNeeded() const {
  return height() + 18;
}

void ChangeBar::SetBusy(bool busy) {
  discard_->setEnabled(!busy);
  save_->setEnabled(!busy);
}

bool ChangeBar::eventFilter(QObject* watched, QEvent* event) {
  if (watched == parentWidget() && event->type() == QEvent::Resize) Place();
  return QFrame::eventFilter(watched, event);
}

void ChangeBar::Place() {
  const QWidget* over = parentWidget();
  move((over->width() - width()) / 2, over->height() - height() - 18);
}

}  // namespace mira_gui
