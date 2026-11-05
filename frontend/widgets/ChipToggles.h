#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>
#include <vector>

class QPushButton;

namespace mira_gui {

// A list setting whose items are picked from a few fixed choices, as a row of chips that each
// turn one choice on or off.
class ChipToggles : public QWidget {
  Q_OBJECT

 public:
  struct Choice {
    QString value;
    QString label;
    QString doc;   // the chip's tooltip
    QString pair;  // the choice in another list that goes with this one
    bool needs_pair = false;
  };
  explicit ChipToggles(QWidget* parent = nullptr);

  void SetChoices(std::vector<Choice> choices);
  const std::vector<Choice>& Choices() const { return choices_; }
  // The chosen values, in the choices' order.
  QStringList Values() const;
  void SetValues(const QStringList& values);
  void SetOn(const QString& value, bool on);
  // Greys out each chip that needs its pair while that pair isn't in `values`.
  void SetPairValues(const QStringList& values);

 signals:
  void Changed();
  // The user turned `value` on or off.
  void Clicked(const QString& value, bool on);

 private:
  std::vector<Choice> choices_;
  std::vector<QPushButton*> chips_;
  QStringList unknown_;  // values no choice offers, kept so saving doesn't drop them
};

}  // namespace mira_gui
