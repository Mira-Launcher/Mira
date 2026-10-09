#include "PackageInstall.h"

#include <QCoreApplication>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#include "../app/Notify.h"
#include "../client/api/Config.h"

namespace mira_gui::system {
namespace {

QStringList Args(const std::vector<std::string>& argv) {
  QStringList args;
  for (const std::string& part : argv) args << QString::fromStdString(part);
  return args;
}

// The first terminal found, running `argv` with sudo and waiting for Enter so its output can be read.
bool OpenTerminal(const QStringList& argv) {
  struct Terminal {
    const char* program;
    QStringList run;  // what comes before the command
  };
  static const Terminal kTerminals[] = {
      {"konsole", {"-e"}},       {"gnome-terminal", {"--"}}, {"ptyxis", {"--"}},       {"kgx", {"--"}},
      {"xfce4-terminal", {"-x"}}, {"mate-terminal", {"-x"}}, {"tilix", {"-e"}},        {"alacritty", {"-e"}},
      {"kitty", {}},             {"foot", {}},             {"wezterm", {"start", "--"}}, {"x-terminal-emulator", {"-e"}},
      {"xterm", {"-e"}},
  };
  const QString script = "sudo \"$@\"; status=$?; printf '\\nPress Enter to close. '; read -r _; exit $status";
  for (const Terminal& terminal : kTerminals) {
    const QString path = QStandardPaths::findExecutable(terminal.program);
    if (path.isEmpty()) continue;
    QProcess process;
    // Mira's own platform choice (xcb in its packages) isn't the terminal's.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove("QT_QPA_PLATFORM");
    process.setProcessEnvironment(env);
    process.setProgram(path);
    process.setArguments(terminal.run + QStringList{"sh", "-c", script, "sh"} + argv);
    if (process.startDetached()) return true;
  }
  return false;
}

}  // namespace

QString Names(const std::vector<std::string>& packages) {
  QStringList names = Args(packages);
  if (names.size() < 2) return names.join(QString());
  const QString last = names.takeLast();
  return names.join(", ") + " and " + last;
}

void InstallPackages(QWidget* parent, const std::vector<std::string>& install, const QString& names, bool restart,
                     std::function<void(bool installed)> done) {
  const QStringList argv = Args(install);
  const QString command = "sudo " + argv.join(' ');
  // Test sandboxes share the real system.
  if (qEnvironmentVariableIsSet("MIRA_NO_POWER")) {
    notify::Info(parent, "Not installed", "Installing system packages is off here (MIRA_NO_POWER).");
    return done(false);
  }
  if (QStandardPaths::findExecutable("pkexec").isEmpty()) {
    if (OpenTerminal(argv)) {
      notify::Info(parent, "Finish in the terminal",
                   "Type your password in the terminal that opened to install " + names + ".");
    } else {
      notify::Info(parent, "Install " + names, "Install it from a terminal:\n\n" + command);
    }
    return done(false);
  }

  QPointer<QWidget> guard(parent);
  auto* process = new QProcess(QCoreApplication::instance());
  process->setProcessChannelMode(QProcess::MergedChannels);
  QObject::connect(process, &QProcess::finished, process,
                   [process, guard, names, command, restart, done = std::move(done)](int code,
                                                                                    QProcess::ExitStatus status) {
    process->deleteLater();
    const QString output = QString::fromUtf8(process->readAll());
    // pkexec's own codes: the prompt was dismissed or the password refused.
    if (status == QProcess::NormalExit && (code == 126 || code == 127)) return done(false);
    if (status != QProcess::NormalExit || code != 0) {
      const QStringList lines = output.split(QRegularExpression("[\\r\\n]"), Qt::SkipEmptyParts);
      notify::FailedWithHint(guard.data(), "Could not install " + names + ".",
                             lines.isEmpty() ? QString() : lines.last().trimmed(),
                             "Install it from a terminal instead: " + command);
      return done(false);
    }
    if (restart) {
      notify::Info(guard.data(), "Restart to finish", names + " is added to the system once you restart.");
      return done(false);
    }
    done(true);
  });
  process->start("pkexec", argv);
}

void EnsurePackages(QWidget* parent, const std::string& feature, const QString& what,
                    std::function<void(bool ready)> done) {
  QPointer<QWidget> guard(parent);
  api::GetSystemPackagesAsync(parent, feature, [guard, what, done = std::move(done)](SystemPackagesResult result) {
    // An older mirad, or a failed check: let the work itself say what's wrong.
    if (!result.ok || result.missing.empty()) return done(true);
    QWidget* parent = guard.data();
    const QString names = Names(result.missing);
    if (result.install.empty()) {
      notify::Info(parent, "Install " + names,
                   what + " needs " + names + ", which isn't installed. Install it with your system's package "
                   "manager, then try again.");
      return done(false);
    }
    QString question = what + " needs " + names + ", which isn't installed. Mira installs it now; your system "
                       "asks for your password.";
    if (result.restart) question += " On " + QString::fromStdString(result.distro) + " it's added to the system "
                                    "image, so restart before installing " + what + ".";
    if (!notify::Confirm(parent, "Install " + names + "?", question, "Install")) return done(false);
    InstallPackages(parent, result.install, names, result.restart, done);
  });
}

}  // namespace mira_gui::system
