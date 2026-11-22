#include "ChipToggles.h"

#include <QPushButton>

#include "FlowLayout.h"

namespace mira_gui {

ChipToggles::ChipToggles(QWidget* parent) : QWidget(parent) {
  new FlowLayout(this, 6);
}

void ChipToggles::SetChoices(std::vector<Choice> choices) {
  for (QPushButton* chip : chips_) delete chip;
  chips_.clear();
  choices_ = std::move(choices);
  for (const Choice& choice : choices_) {
    auto* chip = new QPushButton(choice.label, this);
    chip->setObjectName("step_chip");
    chip->setCheckable(true);
    chip->setToolTip(choice.doc);
    chip->setCursor(Qt::PointingHandCursor);
    connect(chip, &QPushButton::clicked, this, [this, value = choice.value](bool on) {
      emit Clicked(value, on);
      emit Changed();
    });
    layout()->addWidget(chip);
    chips_.push_back(chip);
  }
}

QStringList ChipToggles::Values() const {
  QStringList values;
  for (size_t i = 0; i < choices_.size(); ++i) {
    if (chips_[i]->isChecked()) values << choices_[i].value;
  }
  return values + unknown_;
}

void ChipToggles::SetValues(const QStringList& values) {
  const QStringList before = Values();
  unknown_.clear();
  for (const QString& value : values) {
    if (std::ranges::find(choices_, value, &Choice::value) == choices_.end()) unknown_ << value;
  }
  for (size_t i = 0; i < choices_.size(); ++i)
    chips_[i]->setChecked(values.contains(choices_[i].value));
  if (Values() != before) emit Changed();
}

void ChipToggles::SetOn(const QString& value, bool on) {
  const auto it = std::ranges::find(choices_, value, &Choice::value);
  if (it == choices_.end()) return;
  QPushButton* chip = chips_[size_t(it - choices_.begin())];
  if (chip->isChecked() == on) return;
  chip->setChecked(on);
  emit Changed();
}

void ChipToggles::SetPairValues(const QStringList& values) {
  for (size_t i = 0; i < choices_.size(); ++i) {
    if (choices_[i].needs_pair) chips_[i]->setEnabled(values.contains(choices_[i].pair));
  }
}

}  // namespace mira_gui
