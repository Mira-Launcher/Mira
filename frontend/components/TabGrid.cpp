#include "TabGrid.h"

#include <QGridLayout>
#include <QPushButton>

#include "TabRow.h"

namespace mira_gui {

TabGrid::TabGrid(int columns, QWidget* parent) : QWidget(parent), columns_(columns) {
  grid_ = new QGridLayout(this);
  grid_->setContentsMargins(0, 0, 0, 0);
  grid_->setHorizontalSpacing(4);
  grid_->setVerticalSpacing(2);
}

void TabGrid::AddTab(const QString& key, const QString& label) {
  auto* button = new QPushButton(label, this);
  button->setCheckable(true);
  button->setFlat(true);
  button->setCursor(Qt::PointingHandCursor);
  connect(button, &QPushButton::clicked, this, [this, key] {
    // Clicking the current tab again keeps it current.
    const bool changed = key != current_;
    SetCurrent(key);
    if (changed) emit CurrentChanged(key);
  });
  const int index = static_cast<int>(by_key_.size());
  grid_->addWidget(button, index / columns_, index % columns_);
  grid_->setColumnStretch(index % columns_, 1);
  by_key_.insert(key, Tab{button, label});
}

void TabGrid::SetCount(const QString& key, int count) {
  const auto it = by_key_.find(key);
  if (it != by_key_.end()) it->button->setText(TabLabel(it->label, count));
}

void TabGrid::SetCurrent(const QString& key) {
  current_ = by_key_.contains(key) ? key : QString();
  for (auto it = by_key_.begin(); it != by_key_.end(); ++it)
    it->button->setChecked(it.key() == current_);
}

}  // namespace mira_gui
