#pragma once

class QAbstractScrollArea;
class QWidget;

namespace mira_gui {

// A wheel notch moves `area` about 120 px, and Page Up/Down and Home/End move it while focus
// is anywhere in `scope` (default `area`; a page passes itself so its nav column counts too),
// except in a control that uses those keys itself. A dropdown, number box or slider in it takes
// the wheel only once focused.
void SetUpScrolling(QAbstractScrollArea* area, QWidget* scope = nullptr);

// Lays out `page`, a scroll area's widget, and every layout in it before the next paint. Otherwise
// a grid whose tiles resized moves what's below it a frame late, and the page grows a frame after
// that.
void SettleLayout(QWidget* page);

}  // namespace mira_gui
