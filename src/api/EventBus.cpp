#include "api/EventBus.h"

#include <algorithm>

#include "core/Lane.h"

namespace mira::api {

EventBus::EventBus(size_t capacity)
    : next_id_(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count()),
      capacity_(capacity) {}

void EventBus::SetGameRecordHook(std::function<void(nlohmann::json& game)> hook) {
  std::lock_guard lock(hook_mutex_);
  game_hook_ = std::move(hook);
}

void EventBus::SetArtHook(std::function<nlohmann::json(const std::string& id)> hook) {
  std::lock_guard lock(hook_mutex_);
  art_hook_ = std::move(hook);
}

void EventBus::DecorateGames(const std::string& type, nlohmann::json& payload) {
  if (!payload.is_object()) return;
  if (type == "game.metadata_ready" || type == "game.metadata_failed" || type == "game.artwork_selected") {
    std::function<nlohmann::json(const std::string&)> art;
    {
      std::lock_guard lock(hook_mutex_);
      art = art_hook_;
    }
    if (art && payload.contains("id")) payload["art"] = art(payload.value("id", std::string()));
    return;
  }
  std::function<void(nlohmann::json&)> hook;
  {
    std::lock_guard lock(hook_mutex_);
    hook = game_hook_;
  }
  if (type == "game.state") {
    // A full record gets the rest of a record's fields too.
    if (hook && payload.contains("name")) hook(payload);
    // A state change says outright whether it runs; asking the supervisor
    // could race its own bookkeeping.
    payload["running"] = payload.value("state", std::string()) == "running";
    return;
  }
  if (!hook) return;
  if (type == "game.added" || type == "game.updated") {
    if (payload.contains("id")) hook(payload);
  } else if (type == "games.updated" && payload.contains("games") && payload["games"].is_array()) {
    for (nlohmann::json& game : payload["games"]) hook(game);
  }
}

model::Event EventBus::Publish(std::string type, nlohmann::json payload) {
  DecorateGames(type, payload);
  model::Event event;
  {
    std::lock_guard lock(mutex_);
    event = {next_id_++, model::NowSeconds(), std::move(type), std::move(payload)};
    events_.push_back(event);
    while (events_.size() > capacity_) events_.pop_front();
  }
  cv_.notify_all();
  return event;
}

model::Event EventBus::PublishNotification(model::NotifyLevel level, std::string message,
                                           nlohmann::json extra) {
  extra["level"] = model::ToString(level);
  extra["message"] = std::move(message);
  return Publish("notification", std::move(extra));
}

std::optional<model::Event> EventBus::WaitNext(std::int64_t after_id, const std::atomic<bool>& stop,
                                                std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  const bool got = cv_.wait_for(lock, timeout, [&] {
    return stop.load(std::memory_order_relaxed) ||
           (!events_.empty() && events_.back().id > after_id);
  });
  if (!got || stop.load(std::memory_order_relaxed)) return std::nullopt;
  for (const model::Event& event : events_) {
    if (event.id > after_id) return event;
  }
  return std::nullopt;  // unreachable given the wait predicate, but keeps the type honest
}

void EventBus::WakeWaiters() {
  // Under the lock, so a waiter can't check `stop` and then miss this.
  const std::lock_guard lock(mutex_);
  cv_.notify_all();
}

std::vector<model::Event> EventBus::Since(std::int64_t after_id) const {
  std::lock_guard lock(mutex_);
  std::vector<model::Event> out;
  for (const model::Event& event : events_) {
    if (event.id > after_id) out.push_back(event);
  }
  return out;
}

std::int64_t EventBus::LatestId() const {
  std::lock_guard lock(mutex_);
  return events_.empty() ? 0 : events_.back().id;
}

void AddHintAndFix(nlohmann::json& out, const Error& error) {
  if (!error.hint.empty()) out["hint"] = error.hint;
  if (error.fix.kind.empty()) return;
  nlohmann::json fix = {{"kind", error.fix.kind}, {"target", error.fix.target}};
  if (!error.fix.step.empty()) fix["step"] = error.fix.step;
  out["fix"] = std::move(fix);
}

namespace {

// Never empty: a tool that failed without a word still gets its code read out ("store_failed" -> "store failed").
std::string ErrorMessage(const Error& error) {
  if (!error.message.empty()) return error.message;
  std::string words = error.code.empty() ? std::string("failed") : error.code;
  std::ranges::replace(words, '_', ' ');
  return words;
}

}  // namespace

nlohmann::json ErrorJson(const Error& error) {
  nlohmann::json out = {{"code", error.code}, {"message", ErrorMessage(error)}};
  AddHintAndFix(out, error);
  return out;
}

nlohmann::json FailedEvent(nlohmann::json fields, const Error& error) {
  // A cancelled job's work fails in whatever words its tool used; it was a cancel.
  if (ThisTaskCancelled()) {
    fields["error"] = "Cancelled";
    fields["code"] = "cancelled";
    return fields;
  }
  fields["error"] = ErrorMessage(error);
  fields["code"] = error.code;
  AddHintAndFix(fields, error);
  return fields;
}

}  // namespace mira::api
