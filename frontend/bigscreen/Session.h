#pragma once

class QObject;

namespace mira_gui::bigscreen {

// Writes ~/.config/autostart/mira.desktop when `on`, removes it otherwise.
void ApplyStartOnLogin(bool on);

// Asks mirad to keep Steam's "Mira" shortcut pointing at this build, opening big screen. Only for
// an AppImage or an installed build, so a dev build never ends up in Steam.
void UpdateSteamShortcut(QObject* context);

}  // namespace mira_gui::bigscreen
