#pragma once

#include <QPixmap>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QObject;

namespace mira_gui {

// Fetches a store's image (a screenshot) and calls `done` with it while `context` lives. Only an
// http(s) URL is fetched, so a store can't hand us a file:// one, and a body past 8 MB is dropped
// as it arrives. Nothing is called when it fails or isn't an image.
void FetchWebImage(QNetworkAccessManager* network, const QString& url, QObject* context,
                   std::function<void(const QPixmap&)> done);

}  // namespace mira_gui
