#pragma once

#include <QObject>

namespace mira_gui::bigscreen {

// Writes ~/.config/autostart/mira.desktop when `on`, removes it otherwise.
void ApplyStartOnLogin(bool on);

// Asks mirad to keep Steam's "Mira" shortcut pointing at this build, opening big screen. Only for
// an AppImage or an installed build, so a dev build never ends up in Steam.
void UpdateSteamShortcut(QObject* context);

// "Suspend", "Reboot" or "PowerOff" through logind. False when logind refused.
bool Power(const char* action);

// Keeps the screen from blanking (org.freedesktop.ScreenSaver) until released; 0 when unavailable.
unsigned InhibitScreenBlanking();
void ReleaseScreenBlanking(unsigned cookie);

// Tells when the computer wakes from suspend (logind's PrepareForSleep).
class SleepWatcher : public QObject {
  Q_OBJECT

public:
  explicit SleepWatcher(QObject* parent);

signals:
  void Woke();

private slots:
  void OnPrepareForSleep(bool sleeping);
};

}  // namespace mira_gui::bigscreen
