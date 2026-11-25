#pragma once

// The sizes cards, rows and the settings column share, so everything in them lines up.
namespace mira_gui {

// Every control's height in a card (base.qss), so a row is as tall whatever it holds.
constexpr int kControlHeight = 32;
// The space either side of a row in a card or a popover: before its first part, after its last.
constexpr int kRowPaddingX = 18;
// Where a row's label starts in its card (the line's margin, the dot and the spaces after it), and
// so where a card's title and anything else under it start.
constexpr int kLabelInset = 8 + 6 + 6 + 4;
// A text field in a row, with its buttons (Browse…, Open), so every field starts at one line.
constexpr int kFieldColumnWidth = 240;
// A number box in a row, wide enough for its unit, so a card's boxes line up on both edges.
constexpr int kNumberWidth = 120;
// The gap under a page's title, on the page and in the header that sticks while scrolling.
constexpr int kTitleGap = 12;

// A column of cards on a Settings page or a game's card, at the tuned text size, and the gap
// between two columns.
constexpr int kCardColumn = 516;
constexpr int kColumnGap = 12;
// A column's width at the current text size: wider at a larger one, so labels still fit.
int CardColumnWidth();
// How wide `columns` columns of cards `column_width` px wide are, with the gaps between them.
int ColumnsWidth(int columns, int column_width);

}  // namespace mira_gui
