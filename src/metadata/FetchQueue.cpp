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
  Job job{std::move(game), Kind::Full, {}, announce, {}};
  if (done) job.done.push_back(std::move(done));
  games_.push_back(std::move(job));
  StartWorkers(config, events);
}

int FetchQueue::EnqueueTitles(const config::Config& config, api::EventBus& events,
                              std::vector<model::Game> titles) {
  std::vector<model::Game> others;
  std::vector<model::Game> steam;
  {
    std::lock_guard lock(mutex_);
    for (model::Game& title : titles) {
      if (Queued(title.id)) continue;
      (title.runner_ref.starts_with("steam:") ? steam : others).push_back(std::move(title));
    }
    for (std::size_t at = 0; at < steam.size(); at += kSteamBatch) {
      Job job{steam[at], Kind::SteamTitles, {}, /*announce=*/false, {}};
      job.batch.assign(steam.begin() + static_cast<std::ptrdiff_t>(at),
                       steam.begin() + static_cast<std::ptrdiff_t>(std::min(steam.size(), at + kSteamBatch)));
      titles_.push_back(std::move(job));
    }
  }
  return static_cast<int>(steam.size()) + EnqueueLow(config, events, std::move(others), Kind::Title);
}

std::vector<std::string> FetchQueue::Ids(const Job& job) {
  if (job.batch.empty()) return {job.game.id};
  std::vector<std::string> ids;
  for (const model::Game& game : job.batch) ids.push_back(game.id);
  return ids;
}

bool FetchQueue::Queued(const std::string& id) const {
  return std::any_of(titles_.begin(), titles_.end(), [&id](const Job& job) {
    return job.game.id == id ||
           std::any_of(job.batch.begin(), job.batch.end(), [&id](const model::Game& game) { return game.id == id; });
  });
}

int FetchQueue::EnqueueStale(const config::Config& config, api::EventBus& events, std::vector<model::Game> games) {
  return EnqueueLow(config, events, std::move(games), Kind::Details);
}

int FetchQueue::EnqueueLow(const config::Config& config, api::EventBus& events, std::vector<model::Game> games,
                           Kind kind) {
  std::lock_guard lock(mutex_);
  int queued = 0;
  for (model::Game& game : games) {
    if (Queued(game.id)) continue;
    titles_.push_back({std::move(game), kind, {}, /*announce=*/false, {}});
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
    threads_.Post([this, &config, &events] { Work(config, events); });
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
          next = std::find_if(queue->begin(), queue->end(), [this](const Job& waiting) {
            return std::ranges::none_of(Ids(waiting),
                                        [this](const std::string& id) { return running_ids_.contains(id); });
          });
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
      for (const std::string& id : Ids(job)) running_ids_.insert(id);
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
    for (const std::string& id : Ids(job)) running_ids_.erase(id);
    --running_;
    idle_.notify_all();
  }
}

bool FetchQueue::Run(const config::Config& config, api::EventBus& events, const Job& job) {
  const model::Game& game = job.game;
  if (job.kind == Kind::SteamTitles) {
    const std::vector<Result<void>> results = FetchSteamTitles(config, cache_, job.batch);
    for (std::size_t i = 0; i < job.batch.size(); ++i) {
      const model::Game& title = job.batch[i];
      if (results[i]) {
        events.Publish("library.artwork_ready", {{"source", title.source}, {"ref", title.source_ref}});
      } else {
        events.Publish("library.artwork_failed",
                       api::FailedEvent({{"source", title.source}, {"ref", title.source_ref}}, results[i].error()));
      }
    }
    return true;
  }
  if (job.kind == Kind::Details) {
    if (const Result<void> refreshed = RefreshDetails(config, cache_, game); !refreshed) {
      log::Debug("couldn't refresh {}'s details: {}", game.id, refreshed.error().message);
      return false;
    }
    events.Publish("game.metadata_ready", {{"id", game.id}});
    return true;
  }
  if (job.kind == Kind::Title) {
    if (const Result<void> fetched = FetchTitle(config, cache_, game); !fetched) {
      log::Debug("no cover for {} {}: {}", game.source, game.source_ref, fetched.error().message);
      events.Publish("library.artwork_failed",
                     api::FailedEvent({{"source", game.source}, {"ref", game.source_ref}}, fetched.error()));
      return false;
    }
    events.Publish("library.artwork_ready", {{"source", game.source}, {"ref", game.source_ref}});
    return true;
  }

  if (auto fetched = Fetch(config, cache_, game); !fetched) {
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
