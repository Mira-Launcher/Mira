#include "api/Routes.h"

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

    res.set_header("Cache-Control", "no-cache");
    // The 20s wait only checks whether this client went away.
    res.set_chunked_content_provider(
        "text/event-stream",
        [&s, after_id, replay_end, live_sent](size_t, httplib::DataSink& sink) mutable -> bool {
          if (s.stopping.load(std::memory_order_relaxed)) return false;
          if (!live_sent && after_id >= replay_end) {
            live_sent = true;
            static constexpr std::string_view kLive = "event: stream.live\ndata: {}\n\n";
            return sink.write(kLive.data(), kLive.size());
          }
          auto event = s.events.WaitNext(after_id, s.stopping, std::chrono::milliseconds(20000));
          if (!event) return sink.is_writable() && !s.stopping.load(std::memory_order_relaxed);
          after_id = event->id;
          const std::string frame =
              std::format("id: {}\nevent: {}\ndata: {}\n\n", event->id, event->type,
                         event->payload.dump());
          return sink.write(frame.data(), frame.size());
        });
  });
}

}  // namespace mira::api
