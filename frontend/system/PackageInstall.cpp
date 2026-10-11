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

enum Answer { kClosed, kInstall };

// The install command in a copyable field, with Open terminal, and Install (pkexec) when `offer_install`.
Answer ShowCommand(QWidget* parent, const QString& title, const QString& message, const QString& detail,
                   const QStringList& argv, bool offer_install) {
  PopupDialog dialog(parent, detail.isEmpty() ? notify::Level::Info : notify::Level::Error, title);
  dialog.SetMessage(message);
  if (!detail.isEmpty()) dialog.SetDetail(detail);

  dialog.AddWidget(new CopyField("sudo " + argv.join(' '), &dialog));

  if (offer_install) dialog.AddButton("Install", kInstall, /*default_button=*/true);
  QPushButton* terminal = dialog.AddButton("Open terminal", kClosed, !offer_install);
  QObject::connect(terminal, &QPushButton::clicked, &dialog, [parent, argv] {
    if (!OpenTerminal(argv)) {
      notify::Notice(parent, "No terminal found; copy the command instead.", notify::Level::Warning);
    }
  });
  return dialog.exec() == kInstall ? kInstall : kClosed;
}

}  // namespace

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
  if (QStandardPaths::findExecutable("pkexec").isEmpty()) {
    ShowCommand(parent, title_verb + " " + names, "Run this in a terminal to " + verb + " " + names + ".", QString(),
                argv, false);
    return done(false);
  }

  QPointer<QWidget> guard(parent);
  auto* process = new QProcess(QCoreApplication::instance());
  process->setProcessChannelMode(QProcess::MergedChannels);
  QObject::connect(process, &QProcess::finished, process,
                   [process, guard, names, verb, argv, restart, done = std::move(done)](int code,
                                                                                          QProcess::ExitStatus status) {
    process->deleteLater();
    const QString output = QString::fromUtf8(process->readAll());
    // pkexec's own codes: the prompt was dismissed or the password refused.
    if (status == QProcess::NormalExit && (code == 126 || code == 127)) return done(false);
    if (status != QProcess::NormalExit || code != 0) {
      const QStringList lines = output.split(QRegularExpression("[\\r\\n]"), Qt::SkipEmptyParts);
      ShowCommand(guard.data(), "Could not " + verb + " " + names,
                  "Run it from a terminal instead.", lines.isEmpty() ? QString() : lines.last().trimmed(), argv,
                  false);
      return done(false);
    }
    if (restart) {
      notify::Info(guard.data(), "Restart to finish", "The change to " + names + " takes effect once you restart.");
      return done(false);
    }
    done(true);
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
    const bool pkexec = !QStandardPaths::findExecutable("pkexec").isEmpty();
    QString question = what + " needs " + names + ", which isn't installed. " +
                       (pkexec ? "Install asks for your password." : "Run this in a terminal to install it.");
    if (result.restart) question += " On " + QString::fromStdString(result.distro) + " it's added to the system "
                                    "image, so restart before using " + what + ".";
    if (ShowCommand(parent, "Install " + names + "?", question, QString(), Args(result.install), pkexec) != kInstall)
      return done(false);
    InstallPackages(parent, result.install, names, result.restart, done);
  });
}

}  // namespace mira_gui::system
