#include "CopyField.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace mira_gui {

CopyField::CopyField(const QString& text, QWidget* parent) : QWidget(parent), text_(text) {
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  // Non-breaking hyphens keep "--needed" whole when it wraps; they aren't the shell's, hence Copy.
  auto* field = new QLabel(QString(text).replace('-', QChar(0x2011)), this);
  field->setObjectName("copy_field");
  field->setWordWrap(true);
  copy_ = new QPushButton("Copy", this);
  connect(copy_, &QPushButton::clicked, this, [this] {
    QApplication::clipboard()->setText(text_);
    copy_->setText("Copied");
  });
  layout->addWidget(field, 1);
  layout->addWidget(copy_, 0, Qt::AlignTop);
}

}  // namespace mira_gui
