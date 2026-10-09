#include "WebApps.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include "../client/api/Games.h"

namespace mira_gui::bigscreen {

const std::vector<WebApp>& StreamingApps() {
  static const std::vector<WebApp> apps = {
      {"netflix", "Netflix", "https://www.netflix.com/browse"},
      {"youtube", "YouTube", "https://www.youtube.com/tv"},
      {"disney-plus", "Disney+", "https://www.disneyplus.com/"},
      {"prime-video", "Prime Video", "https://www.primevideo.com/"},
      {"twitch", "Twitch", "https://www.twitch.tv/"},
  };
  return apps;
}

QString KioskBrowser() {
  // Chrome, Brave, Edge and Vivaldi ship Widevine, which Netflix and others need; plain Chromium often doesn't.
  for (const char* name : {"google-chrome-stable", "google-chrome", "brave", "brave-browser", "microsoft-edge-stable",
                           "vivaldi-stable", "chromium", "chromium-browser"}) {
    const QString path = QStandardPaths::findExecutable(name);
    if (!path.isEmpty()) return path;
  }
  return {};
}

void AddWebApp(QObject* context, const WebApp& app, std::function<void(QString)> done) {
  const QString browser = KioskBrowser();
  if (browser.isEmpty()) return done("Streaming apps need Chrome, Brave, Edge, Vivaldi or Chromium.");
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/mira/webapps/" + app.id;
  QDir().mkpath(dir + "/profile");
  // YouTube's TV layout only answers a TV browser.
  const QString agent = app.id == "youtube"
                            ? " --user-agent='Mozilla/5.0 (SMART-TV; Linux; Tizen 6.0) AppleWebKit/538.1 (KHTML, like Gecko) Version/6.0 TV Safari/538.1'"
                            : QString();
  const QByteArray script = QString("#!/bin/sh\n# Added by Mira's big screen.\nexec '%1' --kiosk --app='%2' --user-data-dir='%3/profile' "
                                    "--no-first-run --class='mira-%4'%5\n")
                                .arg(browser, app.url, dir, app.id, agent)
                                .toUtf8();
  QFile file(dir + "/launch.sh");
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(script) != script.size()) {
    return done("Couldn't write " + file.fileName());
  }
  file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
  file.close();
  api::AddManualGameAsync(
      context, dir.toStdString(), "launch.sh", app.name.toStdString(), "native", false,
      [context, done](GameDetailResult added) {
        if (!added.ok) return done(QString::fromStdString(added.error.message));
        GamePatch patch;
        patch.tags = std::vector<std::string>{"app", "media"};
        api::PatchGameAsync(context, added.game.id, patch, [done](PatchGameResult patched) {
          done(patched.ok ? QString() : QString::fromStdString(patched.error.message));
        });
      });
}

}  // namespace mira_gui::bigscreen
