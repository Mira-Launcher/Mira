#include "Session.h"

#include <QCoreApplication>
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
                           Executable().toUtf8() + "\"\nX-GNOME-Autostart-enabled=true\n";
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

}  // namespace mira_gui::bigscreen
