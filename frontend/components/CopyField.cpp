#include "CopyField.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

namespace mira_gui {

CopyField::CopyField(const QString& text, QWidget* parent) : QWidget(parent), text_(text) {
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  // Non-breaking hyphens keep "--needed" whole when it wraps; they aren't the shell's, hence Copy.
  auto* field = new QLabel(QString(text).replace('-', QChar(0x2011)), this);
  field->setObjectName("copy_field");
  field->setWordWrap(true);
  copy_ = new QPushButton("Copy", this);
  auto* reset = new QTimer(this);
  reset->setSingleShot(true);
  reset->setInterval(2000);
  connect(reset, &QTimer::timeout, this, [this] { copy_->setText("Copy"); });
  connect(copy_, &QPushButton::clicked, this, [this, reset] {
    QApplication::clipboard()->setText(text_);
    copy_->setText("Copied");
    reset->start();
  });
  layout->addWidget(field, 1);
  layout->addWidget(copy_, 0, Qt::AlignTop);
}

}  // namespace mira_gui
