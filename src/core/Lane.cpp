#include "core/Lane.h"

#include <exception>

#include "core/Log.h"

namespace mira {
namespace {
thread_local std::stop_token current_stop;
thread_local std::stop_token current_cancel;
}  // namespace

std::stop_token ThisTaskStop() { return current_stop; }

bool ThisTaskCancelled() { return current_cancel.stop_requested(); }

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

Lane::~Lane() { Stop(); }

void Lane::Post(std::function<void()> task) {
  std::lock_guard lock(mutex_);
  if (stopped_) return;
  queue_.push_back(std::move(task));
  const int idle = static_cast<int>(threads_.size()) - busy_;
  if (idle < static_cast<int>(queue_.size()) && static_cast<int>(threads_.size()) < max_workers_) {
    threads_.emplace_back([this](std::stop_token stop) { Work(stop); });
  }
  wake_.notify_one();
}

void Lane::Work(std::stop_token stop) {
  current_stop = stop;
  std::unique_lock lock(mutex_);
  while (true) {
    if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) return;
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
  std::vector<std::jthread> threads;
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    queue_.clear();
    threads = std::move(threads_);
  }
  for (std::jthread& thread : threads) thread.request_stop();
  threads.clear();  // joins
}

void Lane::WaitIdle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return queue_.empty() && busy_ == 0; });
}

}  // namespace mira
