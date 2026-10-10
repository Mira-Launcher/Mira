#include "SourceTile.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QVBoxLayout>

#include "../widgets/Labels.h"

namespace mira_gui {

SourceTile::SourceTile(const SourceInfo& source, QWidget* parent) : QFrame(parent), source_(source) {
  setObjectName("source_tile");
  setAttribute(Qt::WA_StyledBackground, true);
  QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  policy.setHeightForWidth(true);
  setSizePolicy(policy);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(14, 12, 10, 12);
  layout->setSpacing(10);

  auto* head = new QHBoxLayout();
  head->setSpacing(10);
  badge_ = MakeSourceBadge(source, 32, this);
  head->addWidget(badge_, 0, Qt::AlignTop);
  auto* names = new QVBoxLayout();
  names->setSpacing(1);
  name_ = new ElidedLabel(source.name, this);
  name_->setObjectName("source_tile_name");
  line_ = new ElidedLabel(QString(), this);
  line_->setProperty("role", "subtle");
  names->addWidget(name_);
  names->addWidget(line_);
  head->addLayout(names, /*stretch=*/1);
  corner_ = new QHBoxLayout();
  corner_->setSpacing(0);
  head->addLayout(corner_);
  layout->addLayout(head);

  body_ = new QVBoxLayout();
  body_->setSpacing(6);
  layout->addLayout(body_);
}

void SourceTile::SetClickable(const QString& tooltip) {
  clickable_ = true;
  setProperty("clickable", true);
  setCursor(Qt::PointingHandCursor);
  setToolTip(tooltip);
}

void SourceTile::SetDim(bool dim) {
  SetSourceBadgeDim(badge_, source_, dim);
  name_->setProperty("role", dim ? "subtle" : "");
  name_->style()->unpolish(name_);
  name_->style()->polish(name_);
}

bool SourceTile::hasHeightForWidth() const { return layout()->hasHeightForWidth(); }

int SourceTile::heightForWidth(int width) const {
  return layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width) : sizeHint().height();
}

void SourceTile::mousePressEvent(QMouseEvent* event) {
  pressed_at_ = event->globalPosition().toPoint();
  QFrame::mousePressEvent(event);
}

void SourceTile::mouseReleaseEvent(QMouseEvent* event) {
  // A click, not the end of a drag that started on the tile.
  if (clickable_ && event->button() == Qt::LeftButton &&
      (event->globalPosition().toPoint() - pressed_at_).manhattanLength() < 6 && rect().contains(event->position().toPoint())) {
    emit Clicked();
  }
  QFrame::mouseReleaseEvent(event);
}

}  // namespace mira_gui
