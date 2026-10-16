#include "Display.h"

#include <unistd.h>

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
// Lists the variables Mira set rather than the user, so a restart doesn't pass them on as theirs.
constexpr const char* kSetByMira = "MIRA_DISPLAY_ENV";

void SetByMira(const char* name, const QByteArray& value) {
  ::setenv(name, value.constData(), 1);
  QByteArray set = qgetenv(kSetByMira);
  ::setenv(kSetByMira, (set.isEmpty() ? set : set + ',').append(name).constData(), 1);
}

bool IsSetByMira(const char* name) {
  return qgetenv(kSetByMira).split(',').contains(name);
}

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
  if (const char* user = std::getenv("QT_QPA_PLATFORM");
      user != nullptr && !IsSetByMira("QT_QPA_PLATFORM")) {
    return QString::fromUtf8(user);
  }
  const bool wayland = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
  const bool x11 = !qEnvironmentVariableIsEmpty("DISPLAY");
  if (!wayland || (x11 && (big_screen || Server() == "x11" || !WaylandPluginAvailable()))) {
    return "xcb";
  }
  return "wayland";
}

// KDE's scale and mode height per connector, which XWayland screens are named after.
struct KdeOutput {
  double scale = 1;
  int height = 0;
};

QHash<QString, KdeOutput> KdeOutputs() {
  if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
      !qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("KDE")) {
    return {};
  }
  QFile file(ConfigDir() + "/kwinoutputconfig.json");
  if (!file.open(QIODevice::ReadOnly)) return {};
  QHash<QString, KdeOutput> outputs;
  for (const QJsonValue section : QJsonDocument::fromJson(file.readAll()).array()) {
    if (section["name"].toString() != "outputs") continue;
    for (const QJsonValue output : section["data"].toArray()) {
      outputs[output["connectorName"].toString()] = {output["scale"].toDouble(1),
                                                     output["mode"]["height"].toInt()};
    }
  }
  return outputs;
}

// Qt's scale for a screen without Mira's interface size.
double OwnScale(const QScreen* screen) {
  const double factor = qEnvironmentVariable("QT_SCALE_FACTOR", "1").toDouble();
  return screen->devicePixelRatio() / (factor > 0 ? factor : 1);
}

// The scale X11 should give a screen: KDE's for it, or 1080p-sized on a 4K screen when the
// desktop gave none. 0 when Qt's is right, or KDE stretches the window so none is.
double X11Scale(const QScreen* screen, const QHash<QString, KdeOutput>& kde) {
  const double own = OwnScale(screen);
  const int pixels = qRound(screen->geometry().height() * screen->devicePixelRatio());
  double wanted = own;
  if (const auto output = kde.find(screen->name()); output != kde.end()) {
    if (pixels * 1.05 < output->height) return 0;
    wanted = output->scale;
  } else if (own == 1 && pixels >= 2000) {
    wanted = pixels / 1080.0;
  }
  return std::abs(wanted - own) > 0.05 ? wanted : 0;
}

}  // namespace

QString SystemServer() {
  return qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ? "x11" : "wayland";
}

QString Server() {
  const QString server = Store().value("server").toString();
  return server == "wayland" || server == "x11" ? server : "desktop";
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
  if (std::getenv("QT_QPA_PLATFORM") == nullptr)
    SetByMira("QT_QPA_PLATFORM", Wanted(big_screen).toUtf8());
  if (std::getenv("QT_SCALE_FACTOR") == nullptr && Scale() != 1.0) {
    SetByMira("QT_SCALE_FACTOR", QByteArray::number(Scale()));
  }
}

void FixX11Scale(char** argv) {
  if (QGuiApplication::platformName() != "xcb" ||
      !qEnvironmentVariableIsEmpty("QT_SCREEN_SCALE_FACTORS")) {
    return;
  }
  const QHash<QString, KdeOutput> kde = KdeOutputs();
  QByteArrayList factors;
  bool wrong = false;
  for (const QScreen* screen : QGuiApplication::screens()) {
    const double scale = X11Scale(screen, kde);
    wrong = wrong || scale > 0;
    factors << screen->name().toUtf8() + '=' +
                   QByteArray::number(scale > 0 ? scale : OwnScale(screen));
  }
  if (!wrong) return;
  // Qt reads it only as it starts, so start over with it.
  SetByMira("QT_SCREEN_SCALE_FACTORS", factors.join(';'));
  ::execv(QCoreApplication::applicationFilePath().toLocal8Bit().constData(), argv);
}

void ForgetEnv() {
  for (const QByteArray& name : qgetenv(kSetByMira).split(',')) {
    if (!name.isEmpty()) ::unsetenv(name.constData());
  }
  ::unsetenv(kSetByMira);
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
  for (const QByteArray& name : qgetenv(kSetByMira).split(',')) environment.remove(name);
  environment.remove(kSetByMira);
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
  // --desktop: back from big screen, not into it again for big screen at start.
  return Restart(QStringList{big_screen ? "--big-screen" : "--desktop"});
}

QString StretchHint() {
  if (QGuiApplication::platformName() != "xcb") return {};
  const QHash<QString, KdeOutput> kde = KdeOutputs();
  double stretch = 1;
  for (const QScreen* screen : QGuiApplication::screens()) {
    const auto output = kde.find(screen->name());
    if (output == kde.end()) continue;
    const int pixels = qRound(screen->geometry().height() * screen->devicePixelRatio());
    if (pixels > 0 && pixels * 1.05 < output->height) {
      stretch = std::max(stretch, double(output->height) / pixels);
    }
  }
  if (stretch == 1) return {};
  return QString(
             "KDE stretches Mira's X11 window %1× on a scaled screen, which makes it blurry. "
             "Switch the display server to Wayland, or set System Settings › Display & "
             "Monitor › Legacy applications (X11) to Apply scaling themselves.")
      .arg(stretch, 0, 'g', 3);
}

}  // namespace mira_gui::display
