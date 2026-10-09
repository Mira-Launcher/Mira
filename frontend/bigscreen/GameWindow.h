#pragma once

#include <string>

namespace mira_gui::bigscreen {

// Focuses the game's newest window, if it has one open under X11 or XWayland: a window whose
// process sits in the cgroup of the mira-run that runs `game_id`. With `fullscreen`, a plain
// window (not a dialog or splash) is also made full screen, borderless. False when none is open
// yet, the window is Wayland-native, or this build has no xcb.
bool FocusGameWindow(const std::string& game_id, bool fullscreen);

}  // namespace mira_gui::bigscreen
