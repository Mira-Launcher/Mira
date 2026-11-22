#include "Segmented.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPushButton>

namespace mira_gui {

Segmented::Segmented(QWidget* parent) : QFrame(parent) {
  setObjectName("segmented");
  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(3, 3, 3, 3);
  layout_->setSpacing(3);
  group_ = new QButtonGroup(this);
  group_->setExclusive(true);
  connect(group_, &QButtonGroup::buttonClicked, this,
          [this](QAbstractButton* button) { emit Changed(button->property("value").toString()); });
}

Segmented::Segmented(const std::vector<std::pair<QString, QString>>& options, QWidget* parent)
    : Segmented(parent) {
  for (const auto& [value, label] : options) AddOption(value, label);
}

QPushButton* Segmented::AddOption(const QString& value, const QString& label) {
  auto* button = new QPushButton(label, this);
  button->setCheckable(true);
  button->setCursor(Qt::PointingHandCursor);
  button->setProperty("value", value);
  group_->addButton(button);
  layout_->addWidget(button);
  return button;
}

QPushButton* Segmented::Option(const QString& value) const {
  for (QAbstractButton* button : group_->buttons()) {
    if (button->property("value").toString() == value) return static_cast<QPushButton*>(button);
  }
  return nullptr;
}

QString Segmented::Value() const {
  QAbstractButton* checked = group_->checkedButton();
  return checked != nullptr ? checked->property("value").toString() : QString();
}

void Segmented::SetValue(const QString& value) {
  // Unchecking needs exclusivity off for a moment.
  group_->setExclusive(false);
  for (QAbstractButton* button : group_->buttons())
    button->setChecked(button->property("value").toString() == value);
  group_->setExclusive(true);
}

}  // namespace mira_gui
