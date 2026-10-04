#include "EventHub.h"

#include <QCoreApplication>

namespace mira_gui {

EventHub* EventHub::Instance() {
  static auto* hub = new EventHub(QCoreApplication::instance());
  return hub;
}

EventHub::EventHub(QObject* parent) : QObject(parent) {}

void EventHub::Start() {
  if (started_) return;
  started_ = true;
  stream_.Start(
      this,
      [this](std::string type, std::string data) {
        // A resumed connection sends no stream.live: everything it replays is news.
        if (type == "stream.live") live_ = true;
        emit Received(type, data, live_);
        if (live_ && (type == "runners.removed" || type == "runners.download.finished")) emit RunnersChanged();
      },
      [this](bool connected) {
        emit ConnectionChanged(connected);
      });
}

}  // namespace mira_gui
