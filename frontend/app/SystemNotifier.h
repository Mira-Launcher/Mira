#pragma once

#include <QString>

#include <functional>

namespace mira_gui::notify {

// org.freedesktop.Notifications: works on any desktop, keeps history, and
// obeys Do Not Disturb.
namespace system_notifier {

// Persistent: critical urgency, never expires: the user has to see it.
// Transient: normal urgency, the desktop's own default timeout.
enum class Urgency { Transient, Persistent };

// True if mira.desktop is installed in an XDG data dir. Claiming an app id
// that isn't installed makes xdg-desktop-portal warn on every start.
bool DesktopEntryInstalled();

// True if a notification service answered. Queried once and remembered.
bool Available();

// With `action_label`, a button (or a click on the notification) runs
// `on_action`. False if not delivered, so the caller can fall back in-window.
bool Send(Urgency urgency, const QString& summary, const QString& body,
          const QString& action_label = QString(), std::function<void()> on_action = {});

}  // namespace system_notifier
}  // namespace mira_gui::notify
