#pragma once

class QMainWindow;

// One tray icon per process, attached to main.cpp's primary window. Lets a
// launched game keep running (and mirad keep tracking it) with no Mira
// window open: closing the window hides it instead of quitting.
namespace mira_gui::tray {

// False with no tray on the desktop (or Attach() never called).
bool Available();

// Creates the tray icon and makes `window` the managed one (shown/hidden
// together; see IsManaged). No-op past the first call.
void Attach(QMainWindow* window);

// True for the window Attach() was given. A secondary window (opened
// alongside the primary from a menu) is false here and always closes for
// real, since nothing would bring a hidden one back.
bool IsManaged(QMainWindow* window);

// True once Quit was actually chosen (menu or RequestQuit()); it tells a
// managed window's closeEvent "hide" from "let this close for real" apart.
bool Quitting();

// Sets Quitting() true, then closeAllWindows() for a real quit. Both
// Ctrl+Q and the tray menu's Quit call this instead of closeAllWindows()
// directly.
void RequestQuit();

}  // namespace mira_gui::tray
