#pragma once

// The sizes cards, rows and the settings column share, so everything in them lines up.
namespace mira_gui {

// Every control's height in a card (base.qss), so a row is as tall whatever it holds.
constexpr int kControlHeight = 32;
// The space right of a row's controls, to the card's edge.
constexpr int kRowPaddingX = 18;
// Where a row's label starts in its card (the line's margin, the dot and the spaces after it), and
// so where a card's title and anything else under it start.
constexpr int kLabelInset = 8 + 6 + 6 + 4;
// A text field in a row, with its buttons (Browse…, Open), so every field starts at one line.
constexpr int kFieldColumnWidth = 340;
// A number box in a row, wide enough for its unit, so a card's boxes line up on both edges.
constexpr int kNumberWidth = 120;
// The gap under a page's title, on the page and in the header that sticks while scrolling.
constexpr int kTitleGap = 12;

// The settings column's width, and on windows at least kWideRoom wide.
constexpr int kColumn = 720;
constexpr int kWideColumn = 760;
constexpr int kWideRoom = 1400;
// The width of the one column settings and a game's card show their cards in, for `room` px.
int SettingsColumnWidth(int room);

}  // namespace mira_gui
