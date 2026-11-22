#pragma once

#include <QWidget>

class QPushButton;

namespace mira_gui {

// A command (or any text to paste elsewhere) in a monospace box with a Copy button. It wraps
// between words; Copy, not selection, gives the exact text.
class CopyField : public QWidget {
  Q_OBJECT

 public:
  CopyField(const QString& text, QWidget* parent = nullptr);

 private:
  QString text_;
  QPushButton* copy_ = nullptr;
};

}  // namespace mira_gui
