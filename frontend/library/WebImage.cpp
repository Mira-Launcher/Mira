#include "WebImage.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace mira_gui {

namespace {
constexpr qsizetype kMaxImageBytes = 8 * 1024 * 1024;
}  // namespace

void FetchWebImage(QNetworkAccessManager* network, const QString& url, QObject* context,
                   std::function<void(const QPixmap&)> done) {
  const QUrl web(url);
  if (!web.isValid() || (web.scheme() != "https" && web.scheme() != "http")) return;
  QNetworkReply* reply = network->get(QNetworkRequest(web));
  QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                   [reply](qint64 received, qint64) {
                     if (received > kMaxImageBytes) reply->abort();
                   });
  QObject::connect(reply, &QNetworkReply::finished, context, [reply, done = std::move(done)] {
    reply->deleteLater();
    const QByteArray data = reply->readAll();
    QPixmap pixmap;
    if (reply->error() != QNetworkReply::NoError || data.size() > kMaxImageBytes ||
        !pixmap.loadFromData(data))
      return;
    done(pixmap);
  });
}

}  // namespace mira_gui
