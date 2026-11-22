#pragma once

#include <functional>

class QEvent;
class QToolButton;
class QWidget;

// The chevron and click handling shared by everything that folds open in place (a card, an
// expander row).
namespace mira_gui::fold {

// The chevron that folds `owner`, redrawn in a new theme's color.
QToolButton* NewChevron(QWidget* parent, QWidget* owner, std::function<bool()> expanded,
                        std::function<void(bool)> set_expanded);
void ShowChevron(QToolButton* chevron, bool expanded);
// A click released on `head` that folds it: one on a control in it (a switch, a button) is that
// control's.
bool FoldClick(QWidget* head, QEvent* event);

}  // namespace mira_gui::fold
