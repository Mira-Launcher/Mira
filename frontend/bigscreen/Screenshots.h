#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace mira_gui::bigscreen {

// Where a game's screenshots go: ~/Pictures/Mira/<game name, made safe for a folder name>/
QString ScreenshotDir(const QString& game_name);

// Takes a screenshot of the whole screen the game is on, saved as <dir>/<yyyy-MM-dd_HH-mm-ss>.png.
// `done` gets the file path, or an empty path and a short error. Runs the tool asynchronously.
void TakeScreenshot(QObject* context, const QString& game_name,
                    std::function<void(QString path, QString error)> done);

// The game's screenshots, newest first (png/jpg in ScreenshotDir).
QStringList ScreenshotsFor(const QString& game_name);

}  // namespace mira_gui::bigscreen
