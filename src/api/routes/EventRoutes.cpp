#include "api/Routes.h"

#include <atomic>
#include <chrono>
#include <format>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

}  // namespace

void RegisterEventRoutes(httplib::Server& http, Services& s) {
  // --- events (SSE) -----------------------------------------------------

  http.Get("/v1/events", [&s](const Request& req, Response& res) {
    // Each stream holds a server thread for its whole life, so leaked or duplicate clients
    // could otherwise take every thread and leave no request answered, /stop included.
    static constexpr int kMaxStreams = 16;
    static std::atomic<int> streams{0};
    if (streams.fetch_add(1) >= kMaxStreams) {
      streams.fetch_sub(1);
      return SendError(res, 503,
                       Error{"too_many_streams",
                             "too many event streams are open",
                             "Close other Mira windows or `mira events` commands.",
                             {}});
    }
    std::int64_t after_id = 0;
    bool resuming = false;
    if (auto it = req.headers.find("Last-Event-ID"); it != req.headers.end()) {
      after_id = std::atoll(it->second.c_str());
      resuming = true;
    }
    // A new client gets the buffer replayed, then `stream.live` so it can tell
    // history (show it) from news (announce it). A resuming one only missed news.
    const std::int64_t replay_end = resuming ? 0 : s.events.LatestId();
    bool live_sent = resuming;
    // Ids are consecutive, so a jump means the client fell behind the buffer and missed events.
    bool synced = resuming;

    res.set_header("Cache-Control", "no-cache");
    // The 20s wait only checks whether this client went away.
    res.set_chunked_content_provider(
        "text/event-stream",
        [&s, after_id, replay_end, live_sent, synced](size_t,
                                                      httplib::DataSink& sink) mutable -> bool {
          if (s.stopping.load(std::memory_order_relaxed)) return false;
          if (!live_sent && after_id >= replay_end) {
            live_sent = true;
            static constexpr std::string_view kLive = "event: stream.live\ndata: {}\n\n";
            return sink.write(kLive.data(), kLive.size());
          }
          auto event = s.events.WaitNext(after_id, s.stopping, std::chrono::milliseconds(20000));
          if (!event) return sink.is_writable() && !s.stopping.load(std::memory_order_relaxed);
          // `replace`: a payload with bytes that aren't UTF-8 (a game's name, a tool's output) must not throw here.
          std::string frame = std::format("id: {}\nevent: {}\ndata: {}\n\n", event->id, event->type,
                                          event->payload.dump(-1, ' ', false, json::error_handler_t::replace));
          if (synced && event->id > after_id + 1) {
            frame = std::format("event: stream.gap\ndata: {{\"missed\": {}}}\n\n", event->id - after_id - 1) + frame;
          }
          synced = true;
          after_id = event->id;
          return sink.write(frame.data(), frame.size());
        },
        [](bool) { streams.fetch_sub(1); });
  });
}

}  // namespace mira::api
