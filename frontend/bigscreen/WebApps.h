#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <vector>

namespace mira_gui::bigscreen {

struct WebApp {
  QString id;  // folder name, e.g. "netflix"
  QString name;
  QString url;
};

// Streaming sites big screen can add as apps.
const std::vector<WebApp>& StreamingApps();

// The installed Chromium-based browser that runs them full screen, or empty.
QString KioskBrowser();

// Adds `app` to the library: a launcher script in ~/.local/share/mira/webapps/<id>/ that runs
// the browser in kiosk mode with its own profile, tagged app and media. `done` gets an error or "".
void AddWebApp(QObject* context, const WebApp& app, std::function<void(QString error)> done);

}  // namespace mira_gui::bigscreen
