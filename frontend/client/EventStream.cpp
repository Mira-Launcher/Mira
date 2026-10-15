#include "EventStream.h"

#include <httplib.h>

#include <chrono>
#include <thread>
#include <utility>

#include "Async.h"
#include "Transport.h"

namespace mira_gui {
namespace {

// One `event:`/`data:`/`id:` block from the stream. `id` is tracked even for
// a block we don't otherwise act on, so a reconnect resumes from the right
// place rather than replaying from the start of mirad's buffer.
struct SseEvent {
  std::string type;
  std::string data;
  std::string id;
};

SseEvent ParseBlock(const std::string& block) {
  SseEvent event;
  size_t line_start = 0;
  while (line_start < block.size()) {
    size_t line_end = block.find('\n', line_start);
    if (line_end == std::string::npos) line_end = block.size();
    const std::string line = block.substr(line_start, line_end - line_start);
    if (line.rfind("event: ", 0) == 0) {
      event.type = line.substr(7);
    } else if (line.rfind("data: ", 0) == 0) {
      event.data = line.substr(6);
    } else if (line.rfind("id: ", 0) == 0) {
      event.id = line.substr(4);
    }
    line_start = line_end + 1;
  }
  return event;
}

}  // namespace

EventStream::EventStream() : state_(std::make_shared<State>()) {}

// Tells the background thread to stop. It may not notice immediately (it can
// be parked inside a Get() waiting for the next event) but it can no longer
// reach the window that owned it either way (see async::Deliver), so a
// lingering thread is now just idle, not dangerous.
EventStream::~EventStream() {
  state_->stopped.store(true);
}

void EventStream::Start(QObject* context, std::function<void(std::string, std::string)> on_event,
                        std::function<void(bool)> on_connection) {
  QPointer<QObject> guard(context);
  std::thread([guard, state = state_, on_event = std::move(on_event),
               on_connection = std::move(on_connection)]() {
    std::string last_event_id;
    const auto report = [&](bool connected) {
      if (on_connection)
        async::Deliver(guard, [on_connection, connected] { on_connection(connected); });
    };

    while (!state->stopped.load()) {
      httplib::Client client(transport::SocketPath(), 80);
      client.set_address_family(AF_UNIX);
      client.set_connection_timeout(std::chrono::seconds(2));
      client.set_read_timeout(std::chrono::hours(24 * 365));  // events can be arbitrarily far apart

      httplib::Headers headers;
      if (!last_event_id.empty()) headers = {{"Last-Event-ID", last_event_id}};

      std::string buffer;
      bool connected = false;
      client.Get(
          "/v1/events", headers,
          [&](const httplib::Response& response) {
            connected = response.status == 200;
            if (connected) report(true);
            return connected;
          },
          [&](const char* data, size_t length) {
            // Returning false aborts the Get, giving an exit path to a stopped
            // stream that would otherwise block for a year waiting on the next event.
            if (state->stopped.load()) return false;

            buffer.append(data, length);
            size_t block_end;
            while ((block_end = buffer.find("\n\n")) != std::string::npos) {
              const SseEvent event = ParseBlock(buffer.substr(0, block_end));
              buffer.erase(0, block_end + 2);

              if (!event.id.empty()) last_event_id = event.id;
              if (event.type.empty()) continue;
              async::Deliver(guard, [on_event, type = event.type, data = event.data] {
                on_event(type, data);
              });
            }
            return true;
          });

      if (state->stopped.load()) break;
      if (connected) report(false);
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }).detach();
}

}  // namespace mira_gui
