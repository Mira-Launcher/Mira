#pragma once

class QAbstractScrollArea;
class QWidget;

namespace mira_gui {

// A wheel notch moves `area` about 120 px, and Page Up/Down and Home/End move it while focus
// is anywhere in `scope` (default `area`; a page passes itself so its nav column counts too),
// except in a control that uses those keys itself. A dropdown, number box or slider in it never
// takes the wheel.
void SetUpScrolling(QAbstractScrollArea* area, QWidget* scope = nullptr);

// Every wheel notch in the app glides its scroll area to where it would have jumped, and notches
// while it glides add up. Touchpads keep their own pixel steps.
void SetUpSmoothWheel();

// Lays out `page`, a scroll area's widget, and every layout in it before the next paint. Otherwise
// a grid whose tiles resized moves what's below it a frame late, and the page grows a frame after
// that.
void SettleLayout(QWidget* page);

}  // namespace mira_gui
