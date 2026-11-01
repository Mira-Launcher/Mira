#pragma once

#include "DownloadTracker.h"

class QMenu;
class QWidget;

namespace mira_gui {

// Adds Pause, Resume and Cancel for `entry`'s install to a right-click menu, as the Activity panel
// offers them; nothing when `entry` is null or the install can't be paused or cancelled. Returns
// whether it added any.
bool AddInstallActions(QMenu& menu, QWidget* parent, const DownloadTracker& tracker,
                       const DownloadTracker::Entry* entry);

}  // namespace mira_gui
