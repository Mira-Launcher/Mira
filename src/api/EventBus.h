#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <json.hpp>

#include "core/Result.h"
#include "model/Types.h"

namespace mira::api {

// In-memory pub/sub for everything the frontend needs to react to: a game
// detected, provisioning progress, a launch starting. Kept in a capped ring
// buffer rather than persisted: SSE reconnect-and-replay only needs to
// survive the *frontend* restarting, not the daemon, so there is nothing to
// gain from writing every event to disk.
class EventBus {
public:
  // Ids start from the clock, not 1, so a client resuming with an id from
  // before a mirad restart still receives the new daemon's events.
  explicit EventBus(size_t capacity = 500);

  // Assigns the next monotonic id and appends to the ring buffer, waking any
  // blocked subscriber. Thread-safe; called from any thread.
  model::Event Publish(std::string type, nlohmann::json payload);

  // Publishes a "notification" event: the frontend's cue to show a toast or
  // system notification, with the message already decided here rather than
  // reconstructed from other events client-side.
  model::Event PublishNotification(model::NotifyLevel level, std::string message,
                                    nlohmann::json extra = nlohmann::json::object());

  // Blocks until an event past `after_id` exists or `timeout` elapses,
  // whichever comes first. The SSE handler's entire read loop is one call to
  // this: real events are delivered with no added latency, and the bounded
  // timeout is only there so a connection whose client vanished without
  // closing cleanly gets periodically checked rather than parking its thread
  // forever. That check is not an idle-budget wakeup, and it only runs for the
  // lifetime of an actual open connection. Returns nullopt on timeout or if
  // `stop` is set concurrently (shutdown); the caller distinguishes the two
  // via `stop`.
  std::optional<model::Event> WaitNext(std::int64_t after_id, const std::atomic<bool>& stop,
                                       std::chrono::milliseconds timeout);

  // Wakes every WaitNext, so one whose `stop` was just set returns at once.
  void WakeWaiters();

  // Events strictly after `after_id`, for a client reconnecting with
  // Last-Event-ID. May be a partial replay if the id has aged out of the
  // buffer; the frontend is expected to re-fetch state wholesale in that case.
  std::vector<model::Event> Since(std::int64_t after_id) const;

  // The newest event's id, or 0 with none buffered: where a replay ends.
  std::int64_t LatestId() const;

  // Runs on every game record an event carries (game.added, game.updated,
  // each of games.updated's), whoever published it, so every record comes
  // out the same shape. Empty clears it.
  void SetGameRecordHook(std::function<void(nlohmann::json& game)> hook);
  // Gives game.metadata_ready, game.metadata_failed and game.artwork_selected
  // the game's `art`, as its record would show it now. Empty clears it.
  void SetArtHook(std::function<nlohmann::json(const std::string& id)> hook);

private:
  // Applies the two hooks above to `payload`.
  void DecorateGames(const std::string& type, nlohmann::json& payload);

  std::mutex hook_mutex_;
  std::function<void(nlohmann::json& game)> game_hook_;
  std::function<nlohmann::json(const std::string& id)> art_hook_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<model::Event> events_;
  std::int64_t next_id_;
  size_t capacity_;
};

// Adds an error's optional hint and fix to `out` (an error envelope or an event).
void AddHintAndFix(nlohmann::json& out, const Error& error);

// {code, message, hint?, fix?}: an error as every reply, event and job reports it.
nlohmann::json ErrorJson(const Error& error);

// A *.failed event: `fields` plus the error's message (`error`), code, hint and
// fix; inside a cancelled job, just code "cancelled".
nlohmann::json FailedEvent(nlohmann::json fields, const Error& error);

}  // namespace mira::api
