#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <optional>
#include <string>

#include <json.hpp>

#include "api/EventBus.h"
#include "core/Lane.h"
#include "core/Result.h"

namespace mira::api {

// Work too long to hold an HTTP request open for (a scan, an import, moving
// or deleting games): the request answers 202 with the job's id at once, and
// the outcome arrives as events, or from GET /v1/jobs/{id} for a client that
// wasn't listening.
//
// Events: job.started {id, kind, target, label}, job.progress {id, done,
// total, message}, then job.finished {id, kind, target, result} or
// job.failed {id, kind, target, error: {code, message, hint?, fix?}}, whose
// code is "cancelled" after Cancel().
class JobRegistry {
public:
  // What a job's work gets: a way to say how far along it is.
  class Progress {
  public:
    Progress(JobRegistry& registry, std::string id) : registry_(registry), id_(std::move(id)) {}
    void Report(int done, int total, const std::string& message = std::string());

  private:
    JobRegistry& registry_;
    std::string id_;
  };
  using Work = std::function<Result<nlohmann::json>(Progress& progress)>;

  explicit JobRegistry(EventBus& events) : events_(events) {}

  // A valid job token from a client: letters, digits, '-' and '_', 1-64 long.
  static bool IsValidId(const std::string& id);

  // Starts `work` in the background and returns its id: `requested` when the
  // client picked one (so it can listen before this reply lands), else a new
  // one. `label` is a human name for it ("Scanning your library"). Runs on `lane` when given, so work that
  // must not overlap (winetricks in one prefix) shares one, else on the registry's own.
  std::string Start(const std::string& kind, const std::string& target, const std::string& label, Work work,
                    const std::string& requested = std::string(), Lane* lane = nullptr);

  // The job as GET /v1/jobs/{id} shows it, while running and for a while after.
  std::optional<nlohmann::json> Find(const std::string& id) const;

  // Stops a running job: the processes it runs are killed and it ends as
  // job.failed with code "cancelled". Err job_not_found or not_running.
  Result<void> Cancel(const std::string& id);

private:
  void Update(const std::string& id, const std::function<void(nlohmann::json&)>& change);
  std::map<std::string, std::stop_source> cancels_;  // running jobs, under mutex_

  EventBus& events_;
  mutable std::mutex mutex_;
  std::deque<nlohmann::json> jobs_;  // newest last, capped
  std::uint64_t next_ = 0;
  Lane queue_{"jobs", 4};  // declared last: joined before the rest go
};

}  // namespace mira::api
