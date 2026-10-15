#pragma once

#include <QObject>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace mira_gui {

// Long-lived connection to GET /v1/events, streaming Server-Sent Events as
// the daemon does its work. Mirrors `mira watch` (src/cli/main.cpp), a streaming
// GET split on blank lines.
//
// Holds state, with the last event id for replay across reconnects. Background
// thread runs for as long as the connection continues reconnecting, so it is
// an instance rather than a static call.
class EventStream {
 public:
  EventStream();
  ~EventStream();
  EventStream(const EventStream&) = delete;
  EventStream& operator=(const EventStream&) = delete;

  // Starts or restarts the background connection. Delivers on the main thread
  // each event's type and raw JSON data. On drop, reconnects with fixed backoff
  // since mirad may restart independently, replaying via Last-Event-ID so a
  // reconnect doesn't miss events still in mirad's 500-event buffer. Stream stops
  // on destruction. `on_connection` hears each connect (true) and drop (false).
  void Start(QObject* context, std::function<void(std::string type, std::string data)> on_event,
             std::function<void(bool connected)> on_connection = nullptr);

 private:
  // Shared with the background thread rather than owned by it, so that
  // destroying the stream can signal a thread that outlives this object.
  struct State {
    std::atomic<bool> stopped{false};
  };
  std::shared_ptr<State> state_;
};

}  // namespace mira_gui
