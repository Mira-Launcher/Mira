#pragma once

#include <QString>
#include <QStringList>
#include <functional>

class QLocalServer;
class QLockFile;

// Which Qt platform (Wayland or X11) and interface scale Mira runs with. Both are fixed before
// QApplication exists, so they live in ~/.config/mira/display.ini rather than frontend.toml,
// which only mirad can read.
namespace mira_gui::display {

// The desktop session's: "wayland" or "x11".
QString SystemServer();
// "desktop" (the session's), "wayland" or "x11".
QString Server();
void SetServer(const QString& server);
// A factor on top of the desktop's own scale; 1 leaves it as is.
double Scale();
void SetScale(double scale);

// Sets QT_QPA_PLATFORM and QT_SCALE_FACTOR before QApplication, unless the user set them.
// Big screen runs on X11: its toast and keyboard are X11 windows that place themselves.
void Prepare(bool big_screen);
// On X11, starts this process over with QT_SCREEN_SCALE_FACTORS when a screen's scale is off,
// for when the desktop gives X11 no scale or one for all screens. Call right after QApplication.
void FixX11Scale(char** argv);

// Restarting gives up the single-instance lock and activation socket, and hands over a mirad this
// process started.
void SetInstanceLock(QLockFile* lock);
void SetActivationServer(QLocalServer* server);
void SetDaemon(std::function<qint64()> pid, std::function<void()> hand_off);
// Starts mira-gui again with `arguments` and quits this one. False, and nothing given up, when
// it couldn't start.
bool Restart(const QStringList& arguments);
// Restarts into big screen, or back to the desktop, when that needs another platform. False
// when this process can carry on.
bool RestartFor(bool big_screen);

// On KDE under XWayland: a sentence on why Mira looks blurry, when KWin stretches X11 windows
// to a screen's scale. Empty otherwise.
QString StretchHint();

}  // namespace mira_gui::display
