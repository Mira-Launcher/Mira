#include "SectionNav.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QVariantAnimation>
#include <algorithm>

namespace mira_gui {

SectionNav::SectionNav(QScrollArea* scroll, int clearance, QObject* parent)
    : QObject(parent),
      scroll_(scroll),
      clearance_(clearance),
      buttons_(new QButtonGroup(this)),
      glide_(new QVariantAnimation(this)) {
  buttons_->setExclusive(true);
  connect(buttons_, &QButtonGroup::idClicked, this, &SectionNav::Select);
  glide_->setDuration(260);
  glide_->setEasingCurve(QEasingCurve::OutCubic);
  connect(glide_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant& value) { scroll_->verticalScrollBar()->setValue(value.toInt()); });
  connect(glide_, &QVariantAnimation::finished, this, [this] {
    jumping_to_ = -1;
    Sync();
  });
  connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, &SectionNav::Sync);
}

void SectionNav::AddSection(QAbstractButton* button, QWidget* section) {
  button->setCheckable(true);
  sections_.push_back({button, section});
  const int index = static_cast<int>(sections_.size()) - 1;
  buttons_->addButton(button, index);
  if (index == 0) button->setChecked(true);
}

int SectionNav::TopOf(const QWidget* widget) const {
  return widget->mapTo(scroll_->widget(), QPoint(0, 0)).y();
}

void SectionNav::Select(int index) {
  if (index < 0 || index >= Count()) return;
  const Section& section = sections_[index];
  if (section.widget->isHidden()) return;
  if (QLayout* layout = scroll_->widget()->layout()) layout->activate();
  QScrollBar* bar = scroll_->verticalScrollBar();
  const int target = std::clamp(TopOf(section.widget) - clearance_, 0, bar->maximum());
  jumping_to_ = index;
  section.button->setChecked(true);
  glide_->stop();
  glide_->setStartValue(bar->value());
  glide_->setEndValue(target);
  glide_->start();
}

void SectionNav::Stop() {
  glide_->stop();
  jumping_to_ = -1;
}

int SectionNav::CurrentIndex() const {
  const int line = scroll_->verticalScrollBar()->value() + clearance_;
  int current = -1;
  int current_top = 0;
  for (int i = 0; i < Count(); ++i) {
    const QWidget* widget = sections_[i].widget;
    if (widget->isHidden()) continue;
    const int top = TopOf(widget);
    if (current < 0 || (top <= line && (current_top > line || top > current_top))) {
      current = i;
      current_top = top;
    }
  }
  return current;
}

void SectionNav::Sync() {
  const int current = jumping_to_ >= 0 ? jumping_to_ : CurrentIndex();
  if (current >= 0 && !sections_[current].button->isChecked())
    sections_[current].button->setChecked(true);
}

}  // namespace mira_gui
