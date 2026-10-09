#include "Session.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include "../client/api/Library.h"

namespace mira_gui::bigscreen {
namespace {

// The AppImage itself when running from one, not its temporary mount.
QString Executable() {
  const QString appimage = qEnvironmentVariable("APPIMAGE");
  return appimage.isEmpty() ? QCoreApplication::applicationFilePath() : appimage;
}

}  // namespace

void ApplyStartOnLogin(bool on) {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";
  const QString path = dir + "/mira.desktop";
  if (!on) {
    QFile::remove(path);
    return;
  }
  QDir().mkpath(dir);
  const QByteArray entry = "[Desktop Entry]\nType=Application\nName=Mira\nIcon=mira\nExec=\"" +
                           Executable().toUtf8() + "\" --login\nX-GNOME-Autostart-enabled=true\n";
  QFile file(path);
  if (file.open(QIODevice::ReadOnly) && file.readAll() == entry) return;
  file.close();
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(entry);
}

void UpdateSteamShortcut(QObject* context) {
  const QString exe = Executable();
  const bool installed = !qEnvironmentVariable("APPIMAGE").isEmpty() || exe.startsWith("/usr/") || exe.startsWith("/opt/");
  if (!installed) return;
  api::UpdateSteamShortcutAsync(context, exe.toStdString(), "--big-screen");
}

SleepWatcher::SleepWatcher(QObject* parent) : QObject(parent) {
  QDBusConnection::systemBus().connect("org.freedesktop.login1", "/org/freedesktop/login1",
                                       "org.freedesktop.login1.Manager", "PrepareForSleep", this,
                                       SLOT(OnPrepareForSleep(bool)));
}

void SleepWatcher::OnPrepareForSleep(bool sleeping) {
  if (!sleeping) emit Woke();
}

bool Power(const char* action) {
  // Set by test sandboxes, which share the real system bus.
  if (qEnvironmentVariableIsSet("MIRA_NO_POWER")) return false;
  QDBusInterface logind("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                        QDBusConnection::systemBus());
  // interactive: polkit may ask for a password when another user is logged in.
  const QDBusMessage reply = logind.call(action, true);
  return reply.type() != QDBusMessage::ErrorMessage;
}

unsigned InhibitScreenBlanking() {
  QDBusInterface saver("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver");
  const QDBusReply<unsigned> cookie = saver.call("Inhibit", "Mira", "Big screen is open");
  return cookie.isValid() ? cookie.value() : 0;
}

void ReleaseScreenBlanking(unsigned cookie) {
  if (cookie == 0) return;
  QDBusInterface saver("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver");
  saver.call("UnInhibit", cookie);
}

}  // namespace mira_gui::bigscreen
