#include "core/Lane.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <chrono>
#include <exception>

#include "core/Log.h"

namespace mira {
namespace {
constexpr auto kIdleExit = std::chrono::seconds(30);
thread_local std::stop_token current_stop;
thread_local std::stop_token current_cancel;
}  // namespace

std::stop_token ThisTaskStop() {
  return current_stop;
}

bool ThisTaskCancelled() {
  return current_cancel.stop_requested();
}

void RunCancellable(std::stop_token cancel, const std::function<void()>& body) {
  std::stop_source either;
  const std::stop_token outer = current_stop;
  const std::stop_callback on_shutdown(outer, [&either] { either.request_stop(); });
  const std::stop_callback on_cancel(cancel, [&either] { either.request_stop(); });
  current_stop = either.get_token();
  current_cancel = cancel;
  struct Restore {
    std::stop_token stop;
    ~Restore() {
      current_stop = stop;
      current_cancel = {};
    }
  } restore{outer};
  body();
}

Lane::Lane(std::string name, int workers) : name_(std::move(name)), max_workers_(workers) {}

Lane::~Lane() {
  Stop();
}

void Lane::Post(std::function<void()> task) {
  std::list<Worker> finished;
  std::lock_guard lock(mutex_);
  if (stopped_) return;
  for (auto it = workers_.begin(); it != workers_.end();) {
    auto next = std::next(it);
    if (it->done) finished.splice(finished.end(), workers_, it);
    it = next;
  }
  queue_.push_back(std::move(task));
  if (live_ - busy_ < static_cast<int>(queue_.size()) && live_ < max_workers_) {
    Worker& worker = workers_.emplace_back();
    ++live_;
    worker.thread = std::jthread([this, &worker](std::stop_token stop) { Work(stop, &worker); });
  }
  wake_.notify_one();
  // `finished` joins here, after their threads already returned under the lock.
}

void Lane::Work(std::stop_token stop, Worker* self) {
  current_stop = stop;
  std::unique_lock lock(mutex_);
  while (true) {
    if (!wake_.wait_for(lock, stop, kIdleExit, [this] { return !queue_.empty(); })) {
      if (stop.stop_requested()) return;
      self->done = true;
      --live_;
      lock.unlock();
      // Quiet for a while: hand what the work freed back to the system.
#ifdef __GLIBC__
      malloc_trim(0);
#endif
      return;
    }
    std::function<void()> task = std::move(queue_.front());
    queue_.pop_front();
    ++busy_;
    lock.unlock();
    try {
      task();
    } catch (const std::exception& error) {
      log::Error("{} task threw: {}", name_, error.what());
    } catch (...) {
      log::Error("{} task threw a non-std exception", name_);
    }
    lock.lock();
    --busy_;
    if (queue_.empty() && busy_ == 0) idle_.notify_all();
  }
}

void Lane::Stop() {
  std::list<Worker> workers;
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    queue_.clear();
    workers = std::move(workers_);
  }
  for (Worker& worker : workers) worker.thread.request_stop();
  workers.clear();  // joins
}

void Lane::WaitIdle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return queue_.empty() && busy_ == 0; });
}

}  // namespace mira
