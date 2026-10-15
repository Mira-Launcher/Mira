#include "Display.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <cstdlib>
#include <filesystem>

namespace mira_gui::display {
namespace {

QLockFile* instance_lock = nullptr;
QPointer<QLocalServer> activation_server;
std::function<qint64()> daemon_pid;
std::function<void()> hand_off_daemon;
// Set by Prepare rather than by the user, so a restart doesn't pass them on as theirs.
bool set_platform = false;
bool set_scale = false;

QString ConfigDir() {
  const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
  return (xdg.isEmpty() ? QDir::homePath() + "/.config" : xdg);
}

QSettings& Store() {
  static QSettings settings(ConfigDir() + "/mira/display.ini", QSettings::IniFormat);
  return settings;
}

// A bundled Qt (AppImage, .deb, .rpm) may carry only the xcb plugin; a system Qt has both.
bool WaylandPluginAvailable() {
  std::error_code ec;
  const std::filesystem::path platforms =
      std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().parent_path() / "plugins" /
      "platforms";
  if (ec || !std::filesystem::is_directory(platforms, ec)) return true;
  for (const auto& entry : std::filesystem::directory_iterator(platforms, ec)) {
    if (entry.path().filename().string().starts_with("libqwayland")) return true;
  }
  return false;
}

// The platform a process started now would pick: "wayland", "xcb", or the user's own choice.
QString Wanted(bool big_screen) {
  if (const char* user = std::getenv("QT_QPA_PLATFORM"); user != nullptr && !set_platform) {
    return QString::fromUtf8(user);
  }
  const bool wayland = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
  const bool x11 = !qEnvironmentVariableIsEmpty("DISPLAY");
  if (!wayland || (x11 && (big_screen || Server() != "wayland" || !WaylandPluginAvailable()))) {
    return "xcb";
  }
  return "wayland";
}

}  // namespace

QString Server() {
  return Store().value("server", "auto").toString();
}

void SetServer(const QString& server) {
  Store().setValue("server", server);
  Store().sync();
}

double Scale() {
  const double scale = Store().value("scale", 1.0).toDouble();
  return scale >= 0.5 && scale <= 4 ? scale : 1.0;
}

void SetScale(double scale) {
  Store().setValue("scale", scale);
  Store().sync();
}

void Prepare(bool big_screen) {
  if (std::getenv("QT_QPA_PLATFORM") == nullptr) {
    ::setenv("QT_QPA_PLATFORM", Wanted(big_screen).toUtf8().constData(), 1);
    set_platform = true;
  }
  if (std::getenv("QT_SCALE_FACTOR") == nullptr && Scale() != 1.0) {
    ::setenv("QT_SCALE_FACTOR", QByteArray::number(Scale()).constData(), 1);
    set_scale = true;
  }
}

void SetInstanceLock(QLockFile* lock) {
  instance_lock = lock;
}

void SetActivationServer(QLocalServer* server) {
  activation_server = server;
}

void SetDaemon(std::function<qint64()> pid, std::function<void()> hand_off) {
  daemon_pid = std::move(pid);
  hand_off_daemon = std::move(hand_off);
}

bool Restart(const QStringList& arguments) {
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  if (set_platform) environment.remove("QT_QPA_PLATFORM");
  if (set_scale) environment.remove("QT_SCALE_FACTOR");
  // Read by DaemonSupervisor in the new process.
  if (const qint64 pid = daemon_pid ? daemon_pid() : 0; pid > 0) {
    environment.insert("MIRA_DAEMON_PID", QString::number(pid));
  }
  QProcess process;
  process.setProgram(QCoreApplication::applicationFilePath());
  process.setArguments(arguments);
  process.setProcessEnvironment(environment);
  // Closed first: closing it later would remove the new process's socket of the same name.
  const QString server_name = activation_server ? activation_server->serverName() : QString();
  if (activation_server) activation_server->close();
  if (instance_lock != nullptr) instance_lock->unlock();
  if (!process.startDetached()) {
    if (instance_lock != nullptr) instance_lock->tryLock(0);
    if (activation_server) activation_server->listen(server_name);
    return false;
  }
  if (hand_off_daemon) hand_off_daemon();
  QCoreApplication::quit();
  return true;
}

bool RestartFor(bool big_screen) {
  if (QGuiApplication::platformName() == Wanted(big_screen)) return false;
  return Restart(big_screen ? QStringList{"--big-screen"} : QStringList{});
}

QString StretchHint() {
  if (QGuiApplication::platformName() != "xcb" || qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
      !qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE")) {
    return {};
  }
  QFile file(ConfigDir() + "/kwinoutputconfig.json");
  if (!file.open(QIODevice::ReadOnly)) return {};
  QHash<QString, double> scales;
  for (const QJsonValue section : QJsonDocument::fromJson(file.readAll()).array()) {
    if (section["name"].toString() != "outputs") continue;
    for (const QJsonValue output : section["data"].toArray()) {
      scales[output["connectorName"].toString()] = output["scale"].toDouble(1);
    }
  }
  // Under XWayland a screen's ratio is Xft.dpi's, times QT_SCALE_FACTOR.
  const double factor = qEnvironmentVariable("QT_SCALE_FACTOR", "1").toDouble();
  double stretch = 1;
  for (const QScreen* screen : QGuiApplication::screens()) {
    const double kde = scales.value(screen->name(), 1);
    stretch = std::max(stretch, kde / (screen->devicePixelRatio() / (factor > 0 ? factor : 1)));
  }
  if (stretch < 1.05) return {};
  return QString(
             "KDE stretches Mira's X11 window %1× on a scaled screen, which makes it blurry. "
             "Switch the display server to Wayland, or set System Settings › Display & "
             "Monitor › Legacy applications (X11) to Apply scaling themselves.")
      .arg(stretch, 0, 'g', 3);
}

}  // namespace mira_gui::display
