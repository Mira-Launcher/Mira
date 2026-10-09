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

QString Names(const std::vector<std::string>& packages) {
  QStringList names;
  for (const std::string& package : packages) names << QString::fromStdString(package);
  if (names.size() < 2) return names.join(QString());
  const QString last = names.takeLast();
  return names.join(", ") + " and " + last;
}

QString Command(const std::vector<std::string>& argv) {
  QStringList parts;
  for (const std::string& part : argv) parts << QString::fromStdString(part);
  return parts.join(' ');
}

}  // namespace

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
    const QString command = "sudo " + Command(result.install);
    if (QStandardPaths::findExecutable("pkexec").isEmpty()) {
      notify::Info(parent, "Install " + names,
                   what + " needs " + names + ". Install it from a terminal, then try again:\n\n" + command);
      return done(false);
    }
    QString question = what + " needs " + names + ", which isn't installed. Mira installs it now; your system "
                       "asks for your password.";
    if (result.restart) question += " On " + QString::fromStdString(result.distro) + " it's added to the system "
                                    "image, so restart before installing " + what + ".";
    if (!notify::Confirm(parent, "Install " + names + "?", question, "Install")) return done(false);
    // Test sandboxes share the real system.
    if (qEnvironmentVariableIsSet("MIRA_NO_POWER")) {
      notify::Info(parent, "Not installed", "Installing system packages is off here (MIRA_NO_POWER).");
      return done(false);
    }

    auto* process = new QProcess(QCoreApplication::instance());
    process->setProcessChannelMode(QProcess::MergedChannels);
    QStringList argv;
    for (const std::string& part : result.install) argv << QString::fromStdString(part);
    QObject::connect(process, &QProcess::finished, process,
                     [process, guard, names, what, command, restart = result.restart, done](int code,
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
        notify::Info(guard.data(), "Restart to finish",
                     names + " is installed once you restart. Then install " + what + " again.");
        return done(false);
      }
      done(true);
    });
    process->start("pkexec", argv);
  });
}

}  // namespace mira_gui::system
