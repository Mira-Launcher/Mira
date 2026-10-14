#include "PackageInstall.h"

#include <QCoreApplication>
#include <QPointer>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

#include "../app/Notify.h"
#include "../client/api/Config.h"
#include "../widgets/CopyField.h"
#include "../widgets/Labels.h"
#include "../widgets/PopupDialog.h"

namespace mira_gui::system {
namespace {

bool copy_commands = false;

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

// The command in a copyable field, with Open terminal.
void ShowCommand(QWidget* parent, const QString& title, const QString& message, const QString& detail,
                 const QStringList& argv) {
  PopupDialog dialog(parent, detail.isEmpty() ? notify::Level::Info : notify::Level::Error, title);
  dialog.SetMessage(message);
  if (!detail.isEmpty()) dialog.SetDetail(detail);

  dialog.AddWidget(new CopyField("sudo " + argv.join(' '), &dialog));

  QPushButton* terminal = dialog.AddButton("Open terminal", false, true);
  QObject::connect(terminal, &QPushButton::clicked, &dialog, [parent, argv] {
    if (!OpenTerminal(argv)) {
      notify::Notice(parent, "No terminal found; copy the command instead.", notify::Level::Warning);
    }
  });
  dialog.exec();
}

}  // namespace

void SetCopyCommands(bool copy) { copy_commands = copy; }

QString Names(const std::vector<std::string>& packages) { return JoinAnd(Args(packages)); }

namespace {

// Runs `command` as root through pkexec; `verb` ("install", "uninstall") words what it does.
void RunAsRoot(QWidget* parent, const std::vector<std::string>& command, const QString& names, const QString& verb,
               bool restart, std::function<void(bool done)> done) {
  const QStringList argv = Args(command);
  QString title_verb = verb;
  title_verb[0] = title_verb[0].toUpper();
  // Test sandboxes share the real system.
  if (qEnvironmentVariableIsSet("MIRA_NO_POWER")) {
    notify::Info(parent, "Not done", "Changing system packages is off here (MIRA_NO_POWER).");
    return done(false);
  }
  if (copy_commands || QStandardPaths::findExecutable("pkexec").isEmpty()) {
    ShowCommand(parent, title_verb + " " + names, "Run this in a terminal to " + verb + " " + names + ".", QString(),
                argv);
    return done(false);
  }
  // The system's prompt names only the command; this says what the password is for.
  notify::Notice(parent, "Enter your password to " + verb + " " + names + ".");

  QPointer<QWidget> guard(parent);
  auto* process = new QProcess(QCoreApplication::instance());
  process->setProcessChannelMode(QProcess::MergedChannels);
  QObject::connect(process, &QProcess::finished, process,
                   [process, guard, names, verb, argv, restart, done](int code,
                                                                                          QProcess::ExitStatus status) {
    process->deleteLater();
    if (guard.isNull()) return;  // its widget is gone, and with it what `done` would touch
    const QString output = QString::fromUtf8(process->readAll());
    // pkexec's own codes: the prompt was dismissed or the password refused.
    if (status == QProcess::NormalExit && (code == 126 || code == 127)) return done(false);
    if (status != QProcess::NormalExit || code != 0) {
      const QStringList lines = output.split(QRegularExpression("[\\r\\n]"), Qt::SkipEmptyParts);
      ShowCommand(guard.data(), "Could not " + verb + " " + names,
                  "Run it from a terminal instead.", lines.isEmpty() ? QString() : lines.last().trimmed(), argv);
      return done(false);
    }
    if (restart) {
      notify::Info(guard.data(), "Restart to finish", "The change to " + names + " takes effect once you restart.");
      return done(false);
    }
    done(true);
  });
  // No pkexec at all: finished never comes.
  QObject::connect(process, &QProcess::errorOccurred, process, [process, guard, names, verb, argv, done](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;
    process->deleteLater();
    if (guard.isNull()) return;
    ShowCommand(guard.data(), "Could not " + verb + " " + names, "Run it from a terminal instead.", process->errorString(), argv);
    done(false);
  });
  process->start("pkexec", argv);
}

}  // namespace

void InstallPackages(QWidget* parent, const std::vector<std::string>& install, const QString& names, bool restart,
                     std::function<void(bool installed)> done) {
  RunAsRoot(parent, install, names, "install", restart, std::move(done));
}

void UninstallPackages(QWidget* parent, const std::vector<std::string>& uninstall, const QString& names,
                       std::function<void(bool uninstalled)> done) {
  RunAsRoot(parent, uninstall, names, "uninstall", uninstall.front() == "rpm-ostree", std::move(done));
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
    // Straight to the password prompt; RunAsRoot shows the command instead where it must.
    InstallPackages(parent, result.install, names + " for " + what, result.restart, done);
  });
}

}  // namespace mira_gui::system
