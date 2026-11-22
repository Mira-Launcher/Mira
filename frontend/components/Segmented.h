#pragma once

#include <QFrame>
#include <QString>
#include <utility>
#include <vector>

class QButtonGroup;
class QHBoxLayout;
class QPushButton;

namespace mira_gui {

// Two or three choices side by side in one control, the chosen one filled in the accent.
class Segmented : public QFrame {
  Q_OBJECT

 public:
  explicit Segmented(QWidget* parent = nullptr);
  // With these options, each a value and its label.
  Segmented(const std::vector<std::pair<QString, QString>>& options, QWidget* parent);

  QPushButton* AddOption(const QString& value, const QString& label);
  // `value`'s button, null for one that isn't an option.
  QPushButton* Option(const QString& value) const;
  QString Value() const;
  // Selects `value`; one that isn't an option leaves nothing selected. Doesn't emit Changed.
  void SetValue(const QString& value);

 signals:
  // A click picked `value`.
  void Changed(const QString& value);

 private:
  QButtonGroup* group_ = nullptr;
  QHBoxLayout* layout_ = nullptr;
};

}  // namespace mira_gui
