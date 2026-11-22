#include "PageHeader.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "Labels.h"

namespace mira_gui {

PageHeader::PageHeader(const QString& title, QWidget* parent) : QWidget(parent) {
  setObjectName("settings_canvas");
  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(32, 22, 32, 6);
  layout_->setSpacing(10);
  auto* titles = new QVBoxLayout();
  titles->setSpacing(2);
  auto* label = new QLabel(title, this);
  label->setObjectName("page_title");
  titles->addWidget(label);
  subtitle_ = MakeLabel(this, QString(), text::kSubtle);
  subtitle_->hide();
  titles->addWidget(subtitle_);
  titles->addStretch(1);  // the title stays at the top beside a taller control
  layout_->addLayout(titles, /*stretch=*/1);
}

void PageHeader::SetSubtitle(const QString& text) {
  subtitle_->setText(text);
  subtitle_->setVisible(!text.isEmpty());
}

void PageHeader::AddControl(QWidget* control) {
  layout_->addWidget(control, 0, Qt::AlignTop);
}

}  // namespace mira_gui
