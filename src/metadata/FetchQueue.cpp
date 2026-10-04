#include "metadata/FetchQueue.h"

#include <algorithm>
#include <format>

#include "core/Log.h"
#include "metadata/MetadataFetcher.h"

namespace mira::metadata {

FetchQueue::~FetchQueue() {
  // Finishes what's running, drops the rest: a shutdown mid bulk refresh
  // shouldn't wait out hundreds of fetches.
  std::lock_guard lock(mutex_);
  stopping_ = true;
}

void FetchQueue::Enqueue(const config::Config& config, api::EventBus& events, model::Game game, bool force,
                         bool announce, Done done) {
  if (!force && !config.GetBool("metadata.enabled")) return;
  std::lock_guard lock(mutex_);
  const auto waiting = std::find_if(games_.begin(), games_.end(), [&game](const Job& job) {
    return job.game.id == game.id;
  });
  if (waiting != games_.end()) {
    // The newer record (a rename, a new SteamGridDB match) wins.
    waiting->game = std::move(game);
    waiting->announce = waiting->announce || announce;
    if (done) waiting->done.push_back(std::move(done));
    return;
  }
  Job job{std::move(game), /*title=*/false, announce, {}};
  if (done) job.done.push_back(std::move(done));
  games_.push_back(std::move(job));
  StartWorkers(config, events);
}

int FetchQueue::EnqueueTitles(const config::Config& config, api::EventBus& events,
                              std::vector<model::Game> titles) {
  std::lock_guard lock(mutex_);
  int queued = 0;
  for (model::Game& title : titles) {
    if (std::any_of(titles_.begin(), titles_.end(), [&title](const Job& job) { return job.game.id == title.id; })) {
      continue;
    }
    titles_.push_back({std::move(title), /*title=*/true, /*announce=*/false, {}});
    ++queued;
  }
  StartWorkers(config, events);
  return queued;
}

void FetchQueue::WaitIdle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this] { return games_.empty() && titles_.empty() && running_ == 0; });
}

void FetchQueue::StartWorkers(const config::Config& config, api::EventBus& events) {
  const int wanted = std::min<int>(kWorkers, static_cast<int>(games_.size() + titles_.size()) + running_);
  for (; workers_ < wanted; ++workers_) {
    threads_.Run([this, &config, &events] { Work(config, events); });
  }
}

void FetchQueue::Work(const config::Config& config, api::EventBus& events) {
  for (;;) {
    Job job;
    {
      std::lock_guard lock(mutex_);
      // A game already running waits its turn, so two workers never write one game's files.
      std::deque<Job>* from = nullptr;
      std::deque<Job>::iterator next;
      if (!stopping_) {
        for (std::deque<Job>* queue : {&games_, &titles_}) {
          next = std::find_if(queue->begin(), queue->end(),
                              [this](const Job& waiting) { return !running_ids_.contains(waiting.game.id); });
          if (next != queue->end()) {
            from = queue;
            break;
          }
        }
      }
      if (!from) {
        --workers_;
        idle_.notify_all();
        return;
      }
      job = std::move(*next);
      from->erase(next);
      running_ids_.insert(job.game.id);
      ++running_;
    }
    bool ok = false;
    try {
      ok = Run(config, events, job);
    } catch (const std::exception& error) {
      log::Warn("metadata fetch threw for {}: {}", job.game.id, error.what());
    }
    for (const Done& done : job.done) {
      try {
        done(ok);
      } catch (const std::exception& error) {
        log::Warn("metadata fetch callback threw for {}: {}", job.game.id, error.what());
      }
    }
    std::lock_guard lock(mutex_);
    running_ids_.erase(job.game.id);
    --running_;
    idle_.notify_all();
  }
}

bool FetchQueue::Run(const config::Config& config, api::EventBus& events, const Job& job) {
  const model::Game& game = job.game;
  if (job.title) {
    if (const Result<void> fetched = FetchCover(config, game); !fetched) {
      log::Debug("no cover for {} {}: {}", game.source, game.source_ref, fetched.error().message);
      events.Publish("library.artwork_failed",
                     api::FailedEvent({{"source", game.source}, {"ref", game.source_ref}}, fetched.error()));
      return false;
    }
    events.Publish("library.artwork_ready", {{"source", game.source}, {"ref", game.source_ref}});
    return true;
  }

  if (auto fetched = Fetch(config, game); !fetched) {
    const Error& error = fetched.error();
    log::Warn("metadata fetch failed for {}: {}", game.id, error.message);
    events.Publish("game.metadata_failed", api::FailedEvent({{"id", game.id}}, error));
    if (job.announce && fetched.error().code != "no_steamgriddb_key") {
      events.PublishNotification(model::NotifyLevel::Warning,
                                 std::format("No metadata found for \"{}\": {}", game.name,
                                             fetched.error().message.empty() ? "nothing matched this game"
                                                                             : fetched.error().message));
    }
    return false;
  }
  events.Publish("game.metadata_ready", {{"id", game.id}});
  return true;
}

}  // namespace mira::metadata
