#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace mira {

// A named pool of at most `workers` threads running tasks in order. Threads start on first use and then park.
// Stop() drops queued tasks, signals running ones through ThisTaskStop() and joins them.
class Lane {
public:
  Lane(std::string name, int workers);
  ~Lane();
  Lane(const Lane&) = delete;
  Lane& operator=(const Lane&) = delete;

  void Post(std::function<void()> task);
  void Stop();
  // Blocks until nothing is queued or running. For tests.
  void WaitIdle();

private:
  void Work(std::stop_token stop);

  std::string name_;
  int max_workers_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::condition_variable idle_;
  std::deque<std::function<void()>> queue_;
  int busy_ = 0;
  bool stopped_ = false;
  std::vector<std::jthread> threads_;
};

// The stop token of the Lane task running on this thread, or one that never stops elsewhere.
std::stop_token ThisTaskStop();

// Runs `body` with ThisTaskStop() also stopping once `cancel` does, for one
// job that can be cancelled on its own.
void RunCancellable(std::stop_token cancel, const std::function<void()>& body);
// Whether ThisTaskStop() stopped because the task was cancelled, not because mirad is shutting down.
bool ThisTaskCancelled();

}  // namespace mira
