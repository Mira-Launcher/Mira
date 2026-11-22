#pragma once

#include <QPushButton>

namespace mira_gui {

// What a button does, which sets its look: Secondary for most, Primary for the one a card or step
// is for, Danger for one that deletes, Link and Text for quiet actions in running text or a foot
// (Link in the accent, Text muted), Glass over a game's art. Which button Enter presses is separate
// (setDefault), so a Keep can be the default without looking like the action.
enum class ButtonKind { Secondary, Primary, Danger, Link, Text, Glass };

class Button : public QPushButton {
  Q_OBJECT

 public:
  explicit Button(const QString& text = {}, ButtonKind kind = ButtonKind::Secondary,
                  QWidget* parent = nullptr);

  void SetKind(ButtonKind kind);
  ButtonKind Kind() const { return kind_; }

 private:
  ButtonKind kind_ = ButtonKind::Secondary;
};

}  // namespace mira_gui
