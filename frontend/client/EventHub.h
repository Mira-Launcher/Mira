#pragma once

#include <QObject>

#include <string>

#include "EventStream.h"

namespace mira_gui {

// The app's one connection to GET /v1/events, shared by every window, page
// and dialog. A connection of their own would replay mirad's whole buffer to
// each of them, and old outcomes would read as news.
class EventHub : public QObject {
  Q_OBJECT

public:
  static EventHub* Instance();

  // Connects on the first call; later calls do nothing.
  void Start();

signals:
  // Every event, replayed history included. `live` is false for history.
  void Received(const std::string& type, const std::string& data, bool live);
  // A (re)connect or a drop. A reconnect's events resume where the drop left off.
  void ConnectionChanged(bool connected);
  // A runner build was installed or removed, live: runner pickers list them again.
  void RunnersChanged();

private:
  explicit EventHub(QObject* parent);

  EventStream stream_;
  bool started_ = false;
  bool live_ = false;
};

}  // namespace mira_gui
