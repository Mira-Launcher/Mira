#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

class QWidget;

namespace mira_gui::popup {

// The floating card hover cards and tooltips draw as, and where it goes, so every popup has the
// same padding, corners and gap.
inline constexpr int kPaddingX = 12;
inline constexpr int kPaddingY = 10;
inline constexpr int kGap = 8;  // between the popup and what it describes

// The card's rounded surface over all of `widget`, from its paintEvent.
void Paint(QWidget* widget);
// Global top-left for a popup of `size` next to `anchor` (global). `beside`:
// right of it, else left. Otherwise below it, else above. Kept on screen.
QPoint Place(const QRect& anchor, QSize size, bool beside);
// The same, kept inside `area` (global) instead of the screen.
QPoint Place(const QRect& anchor, QSize size, bool beside, const QRect& area);

}  // namespace mira_gui::popup
