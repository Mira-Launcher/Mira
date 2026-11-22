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

// Which edge of its anchor a popover lines up with: its left edge, or its right edge.
enum class Align { Start, End };
// Global top-left for a popover of `size` `gap` px under `anchor` (global), its `align` edge on
// the anchor's, or above it when there's no room below. Kept inside `area` (global).
QPoint PlaceBelow(const QRect& anchor, QSize size, Align align, int gap, const QRect& area);
// The same, kept on the anchor's screen.
QPoint PlaceBelow(const QRect& anchor, QSize size, Align align, int gap);

}  // namespace mira_gui::popup
