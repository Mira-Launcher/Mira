#include "NavRow.h"

#include <QHBoxLayout>
#include <QLabel>

namespace mira_gui {

NavRow::NavRow(const QString& text, QWidget* parent) : QPushButton(text, parent) {
  setFlat(true);
}

QLabel* NavRow::Note() {
  if (note_ != nullptr) return note_;
  note_ = new QLabel(this);
  note_->setAttribute(Qt::WA_TransparentForMouseEvents);
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 10, 0);
  layout->addStretch(1);
  layout->addWidget(note_);
  return note_;
}

}  // namespace mira_gui
