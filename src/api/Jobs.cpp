#include "api/Jobs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <format>

#include "core/Log.h"

namespace mira::api {
namespace {

using nlohmann::json;

// Finished jobs kept for GET /v1/jobs/{id}, oldest dropped first.
constexpr std::size_t kKeptJobs = 100;

}  // namespace

bool JobRegistry::IsValidId(const std::string& id) {
  return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_';
  });
}

void JobRegistry::Progress::Report(int done, int total, const std::string& message) {
  registry_.Update(id_, [&](json& job) { job["progress"] = {{"done", done}, {"total", total}, {"message", message}}; });
  registry_.events_.Publish("job.progress", {{"id", id_}, {"done", done}, {"total", total}, {"message", message}});
}

std::string JobRegistry::Start(const std::string& kind, const std::string& target, const std::string& label,
                               Work work, const std::string& requested, Lane* lane) {
  std::string id;
  {
    std::lock_guard lock(mutex_);
    const bool taken = std::ranges::any_of(jobs_, [&](const json& job) { return job.value("id", "") == requested; });
    id = (IsValidId(requested) && !taken) ? requested
                              : std::format("{}-{}-{}", kind,
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count(),
                                            next_++);
    jobs_.push_back({{"id", id}, {"kind", kind}, {"target", target}, {"label", label}, {"state", "running"}});
    cancels_[id] = std::stop_source();
    // Oldest ended job first: a running one must stay findable.
    while (jobs_.size() > kKeptJobs) {
      const auto ended = std::ranges::find_if(jobs_, [](const json& job) { return job.value("state", "") != "running"; });
      if (ended == jobs_.end()) break;
      jobs_.erase(ended);
    }
  }
  const json identity = {{"id", id}, {"kind", kind}, {"target", target}};
  json started = identity;
  started["label"] = label;
  events_.Publish("job.started", started);

  (lane != nullptr ? *lane : queue_).Post([this, id, identity, work = std::move(work)] {
    Progress progress(*this, id);
    std::stop_source cancel;
    {
      std::lock_guard lock(mutex_);
      cancel = cancels_[id];
    }
    Result<json> result;
    // Caught here so the job still ends: the queue would only log it.
    RunCancellable(cancel.get_token(), [&] {
      try {
        result = work(progress);
      } catch (const std::exception& e) {
        result = std::unexpected(Error{"internal_error", e.what(), "", {}});
      }
    });
    {
      std::lock_guard lock(mutex_);
      cancels_.erase(id);
    }
    // However the work reported it, a cancelled job ends as one.
    if (cancel.stop_requested() && !result) result = std::unexpected(Error{"cancelled", "Cancelled", "", {}});
    json event = identity;
    if (result) {
      event["result"] = *result;
      Update(id, [&](json& job) {
        job["state"] = "finished";
        job["result"] = *result;
      });
      events_.Publish("job.finished", std::move(event));
    } else {
      log::Warn("{} {} failed: {}", identity.value("kind", ""), identity.value("target", ""), result.error().message);
      event["error"] = ErrorJson(result.error());
      Update(id, [&](json& job) {
        job["state"] = "failed";
        job["error"] = ErrorJson(result.error());
      });
      events_.Publish("job.failed", std::move(event));
    }
  });
  return id;
}

Result<void> JobRegistry::Cancel(const std::string& id) {
  std::lock_guard lock(mutex_);
  if (const auto found = cancels_.find(id); found != cancels_.end()) {
    found->second.request_stop();
    return {};
  }
  const bool known = std::ranges::any_of(jobs_, [&](const json& job) { return job.value("id", "") == id; });
  return known ? Err("not_running", "the job has already ended") : Err("job_not_found", "no such job");
}

std::optional<json> JobRegistry::Find(const std::string& id) const {
  std::lock_guard lock(mutex_);
  const auto found = std::ranges::find_if(jobs_, [&](const json& job) { return job.value("id", "") == id; });
  if (found == jobs_.end()) return std::nullopt;
  return *found;
}

void JobRegistry::Update(const std::string& id, const std::function<void(json&)>& change) {
  std::lock_guard lock(mutex_);
  const auto found = std::ranges::find_if(jobs_, [&](const json& job) { return job.value("id", "") == id; });
  if (found != jobs_.end()) change(*found);
}

}  // namespace mira::api
