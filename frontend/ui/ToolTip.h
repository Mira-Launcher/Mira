#pragma once

class QWidget;

namespace mira_gui::tooltip {

// Replaces Qt's tooltips app-wide with a card that looks like HoverCard's,
// placed under the hovered widget (beside a menu item) instead of wherever
// the cursor happens to be. Menu actions' own tooltips show too.
void Install();

// `widget`'s tooltip starts at `edge`'s left side instead of centering under `widget`.
void AlignLeftWith(QWidget* widget, QWidget* edge);

}  // namespace mira_gui::tooltip
