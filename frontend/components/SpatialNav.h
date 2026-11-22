#pragma once

class QWidget;

// Arrow keys that the focused control doesn't use move focus to the nearest control in that
// direction, by where things are on screen rather than by tab order.
namespace mira_gui::spatial_nav {

// Watches every window's keys; call once, after the QApplication exists.
void Install();

// Moves focus from `focus` (null: the window's first control) the way `arrow` points. False when
// nothing lies that way.
bool Move(QWidget* window, QWidget* focus, int arrow);

// Tab goes through a window by where things are too: each container's controls together, in rows
// top to bottom and left to right. A widget marked here (window chrome, like the top bar) comes
// after everything beside it instead.
void TabLast(QWidget* widget);

}  // namespace mira_gui::spatial_nav
