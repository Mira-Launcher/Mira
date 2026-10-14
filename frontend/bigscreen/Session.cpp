#include "Session.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
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

bool ApplyStartOnLogin(bool on) {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";
  const QString path = dir + "/mira.desktop";
  if (!on) return !QFile::exists(path) || QFile::remove(path);
  QDir().mkpath(dir);
  // Inside Exec's quotes ", `, $ and \ take a backslash, which the key's string escaping doubles.
  QString exec;
  for (const QChar c : Executable()) {
    if (c == '"' || c == '`' || c == '$') exec += "\\\\";
    if (c == '\\') exec += "\\\\\\";
    if (c == '%') exec += '%';
    exec += c;
  }
  const QByteArray entry = "[Desktop Entry]\nType=Application\nName=Mira\nIcon=mira\nExec=\"" + exec.toUtf8() +
                           "\" --login\nX-GNOME-Autostart-enabled=true\n";
  QFile file(path);
  if (file.open(QIODevice::ReadOnly) && file.readAll() == entry) return true;
  file.close();
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(entry) == entry.size();
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

void Power(QObject* context, const char* action, std::function<void(bool)> done) {
  // Set by test sandboxes, which share the real system bus.
  if (qEnvironmentVariableIsSet("MIRA_NO_POWER")) return done(false);
  QDBusInterface logind("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                        QDBusConnection::systemBus());
  // interactive: polkit may ask for a password when another user is logged in.
  auto* watcher = new QDBusPendingCallWatcher(logind.asyncCall(action, true), context);
  QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [watcher, done = std::move(done)] {
    watcher->deleteLater();
    done(!watcher->isError());
  });
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
