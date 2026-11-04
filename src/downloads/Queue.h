#pragma once

#include <filesystem>
#include <json.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Lane.h"
#include "core/Result.h"
#include "downloads/Aria2.h"
#include "downloads/Links.h"
#include "store/GameStore.h"

namespace mira::downloads {

struct QueuedFile {
  std::string url;
  std::string name;
  std::string source;
  std::string gid;  // while aria2 has it
  FileStatus status;
};

// One game's links. Downloaded into a hidden folder in the first Mira folder, extracted there, then
// moved into that Mira folder, where the normal sorting and install take over.
struct Download {
  std::string id;
  std::string name;
  std::vector<std::string> links;
  // queued, resolving, service, downloading, paused, extracting, password, failed, done
  std::string state;
  Error error;
  std::vector<QueuedFile> files;
  std::string password;           // tried first; from the confirm step or the prompt
  bool use_service = false;       // a direct download failed: get everything through the service
  std::vector<std::string> jobs;  // what the service is fetching for this one
  std::string service;            // that service's name
  double service_progress = 0;    // 0 to 1, over `jobs`
  bool polling = false;
};

// Runs the download queue. Survives restarts: it's saved on every state change, and what was in
// flight starts again, continuing what aria2 already wrote.
class Queue {
 public:
  Queue(config::Config& config, store::GameStore& games, api::EventBus& events);
  ~Queue();

  void Start();

  nlohmann::json List() const;
  // The files `links` stand for, for the confirm step: their names, sizes and sources.
  Result<nlohmann::json> Check(const std::vector<std::string>& links) const;
  Result<nlohmann::json> Add(const std::string& name, const std::vector<std::string>& links,
                             const std::string& password);
  Result<void> Pause(const std::string& id);
  Result<void> Resume(const std::string& id);
  // Removes it and everything it downloaded. A finished one only leaves the list.
  Result<void> Remove(const std::string& id);
  // Moves a queued one ahead of the others waiting.
  Result<void> MoveToFront(const std::string& id);
  // Tries `password` on one waiting for it; download.changed says how it went.
  Result<void> SubmitPassword(const std::string& id, const std::string& password);
  // Starts a failed one again.
  Result<void> Retry(const std::string& id);

  std::vector<std::string> Passwords() const;
  Result<void> SetPasswords(const std::vector<std::string>& passwords);

 private:
  void Run(std::stop_token stop);
  void Tick();
  void StartNext();
  void PollFiles(Download& download);
  void Fetch(const std::string& id);
  void PollService(const std::string& id);
  void StartFiles(Download& download);
  void Finish(const std::string& id);
  Result<void> Extract(const Download& download, const std::filesystem::path& dir);
  void SetState(Download& download, std::string state, Error error = {});
  Download* FindLocked(const std::string& id);
  void Announce(const Download& download);
  void Save();
  std::filesystem::path WorkDir(const Download& download) const;
  void RememberPassword(const std::string& password);

  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
  mutable std::mutex mutex_;
  std::vector<Download> downloads_;
  Aria2 aria2_;
  std::int64_t applied_limit_ = -1;
  int ticks_ = 0;
  Lane work_{"downloads", 4};
  std::jthread thread_;
};

// For the API.
nlohmann::json ToJson(const Download& download);

}  // namespace mira::downloads
