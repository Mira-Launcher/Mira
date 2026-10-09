#include "Screenshots.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <memory>

namespace mira_gui::bigscreen {
namespace {

constexpr int kTimeoutMs = 15000;

QString PicturesDir() {
  QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  return pictures.isEmpty() ? QDir::homePath() + "/Pictures" : pictures;
}

QString SafeFolderName(const QString& name) {
  QString safe = name;
  for (QChar c : QString("/\\:*?\"<>|")) safe.replace(c, '_');
  for (QChar& c : safe) {
    if (c.isNull() || c.category() == QChar::Other_Control) c = '_';
  }
  safe = safe.trimmed();
  return safe.isEmpty() ? QStringLiteral("Game") : safe;
}

struct Tool {
  QString program;
  QStringList args;  // Ends with the output path.
};

// Picks the first screenshot tool on this system, or returns an empty program.
Tool FindTool(const QString& path) {
  if (!QStandardPaths::findExecutable("spectacle").isEmpty())
    return {"spectacle", {"--background", "--nonotify", "--current", "--output", path}};
  if (!QStandardPaths::findExecutable("grim").isEmpty()) return {"grim", {path}};
  if (!QStandardPaths::findExecutable("gnome-screenshot").isEmpty())
    return {"gnome-screenshot", {"-f", path}};
  if (qEnvironmentVariable("XDG_SESSION_TYPE") != "wayland" &&
      !QStandardPaths::findExecutable("import").isEmpty())
    return {"import", {"-window", "root", path}};
  return {};
}

}  // namespace

QString ScreenshotDir(const QString& game_name) {
  return PicturesDir() + "/Mira/" + SafeFolderName(game_name);
}

void TakeScreenshot(QObject* context, const QString& game_name,
                    std::function<void(QString path, QString error)> done) {
  const QString dir = ScreenshotDir(game_name);
  QDir().mkpath(dir);
  const QString path =
      dir + "/" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") + ".png";

  const Tool tool = FindTool(path);
  if (tool.program.isEmpty()) {
    done({}, "No screenshot tool found. Install Spectacle, grim or gnome-screenshot.");
    return;
  }

  auto* process = new QProcess(context);
  auto reported = std::make_shared<bool>(false);
  auto report = [reported, done](const QString& p, const QString& error) {
    if (*reported) return;
    *reported = true;
    done(p, error);
  };

  QObject::connect(process, &QProcess::finished, process,
                   [process, path, report](int code, QProcess::ExitStatus status) {
                     const bool ok = status == QProcess::NormalExit && code == 0 &&
                                     QFileInfo(path).size() > 0;
                     report(ok ? path : QString(),
                            ok ? QString() : QStringLiteral("The screenshot tool failed."));
                     process->deleteLater();
                   });

  QTimer::singleShot(kTimeoutMs, process, [process, report] {
    if (process->state() == QProcess::NotRunning) return;
    process->kill();
    report({}, "The screenshot tool didn't finish.");
  });

  process->start(tool.program, tool.args);
}

QStringList ScreenshotsFor(const QString& game_name) {
  QDir dir(ScreenshotDir(game_name));
  const QFileInfoList files = dir.entryInfoList({"*.png", "*.jpg", "*.jpeg"}, QDir::Files,
                                                QDir::Time);
  QStringList paths;
  for (const QFileInfo& file : files) paths << file.absoluteFilePath();
  return paths;
}

}  // namespace mira_gui::bigscreen
