#include "downloads/Queue.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <stop_token>

#include "core/Log.h"
#include "core/Paths.h"
#include "downloads/Service.h"
#include "library/ArchiveExtractor.h"
#include "runner/Exec.h"

namespace mira::downloads {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

constexpr const char* kQueueKey = "downloads.queue";
constexpr const char* kPasswordsKey = "downloads.passwords";

std::string NewId() {
  std::random_device random;
  return std::format("{:08x}{:08x}", random(), random());
}

// A name as one folder name.
std::string FolderName(std::string name) {
  std::ranges::replace(name, '/', '-');
  const auto first = name.find_first_not_of(". ");
  return first == std::string::npos ? std::string() : name.substr(first);
}

bool IsLive(const std::string& state) {
  return state == "resolving" || state == "service" || state == "downloading" ||
         state == "extracting";
}

constexpr int kServicePollTicks = 5;

Error ServiceNeeded() {
  return {"service_needed", "torrents need a download service",
          "Add one in Settings, under Downloads."};
}

std::vector<std::string> ParseStrings(const std::string& text) {
  const json parsed = json::parse(text, nullptr, false);
  std::vector<std::string> out;
  if (!parsed.is_array()) return out;
  for (const json& item : parsed) {
    if (item.is_string()) out.push_back(item.get<std::string>());
  }
  return out;
}

}  // namespace

json ToJson(const Download& download) {
  json files = json::array();
  std::int64_t total = 0;
  std::int64_t done = 0;
  std::int64_t speed = 0;
  for (const QueuedFile& file : download.files) {
    total += file.status.total;
    done += file.status.done;
    speed += file.status.speed;
    files.push_back({{"name", file.name},
                     {"source", file.source},
                     {"total", file.status.total},
                     {"done", file.status.done},
                     {"state", file.status.state}});
  }
  json out = {{"id", download.id},
              {"name", download.name},
              {"links", download.links},
              {"state", download.state},
              {"files", files},
              {"total", total},
              {"done", done},
              {"speed", speed},
              {"password", download.password}};
  if (download.state == "service")
    out["service"] = {{"name", download.service}, {"progress", download.service_progress}};
  if (!download.error.code.empty())
    out["error"] = {{"code", download.error.code},
                    {"message", download.error.message},
                    {"hint", download.error.hint}};
  return out;
}

Queue::Queue(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Queue::~Queue() {
  thread_ = {};
  work_.Stop();
}

void Queue::Start() {
  const json saved = json::parse(games_.Secret(kQueueKey), nullptr, false);
  {
    std::lock_guard lock(mutex_);
    if (saved.is_array()) {
      for (const json& item : saved) {
        Download download;
        download.id = item.value("id", std::string());
        download.name = item.value("name", std::string());
        download.state = item.value("state", std::string("queued"));
        download.password = item.value("password", std::string());
        download.use_service = item.value("use_service", false);
        if (item.contains("links")) download.links = item["links"].get<std::vector<std::string>>();
        if (item.contains("error")) {
          download.error.code = item["error"].value("code", std::string());
          download.error.message = item["error"].value("message", std::string());
          download.error.hint = item["error"].value("hint", std::string());
        }
        // What was running starts over and continues what's on disk. One already fetched extracts
        // again.
        if (IsLive(download.state)) download.state = "queued";
        if (!download.id.empty() && download.state != "done")
          downloads_.push_back(std::move(download));
      }
    }
  }
  thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

void Queue::Run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    Tick();
    std::mutex wait_mutex;
    std::unique_lock wait(wait_mutex);
    std::condition_variable_any().wait_for(wait, stop, std::chrono::seconds(1),
                                           [] { return false; });
  }
}

void Queue::Tick() {
  std::lock_guard lock(mutex_);
  for (Download& download : downloads_) {
    if (download.state == "downloading") PollFiles(download);
  }
  if (++ticks_ >= kServicePollTicks) {
    ticks_ = 0;
    for (Download& download : downloads_) {
      if (download.state != "service" || download.polling) continue;
      download.polling = true;
      work_.Post([this, id = download.id] { PollService(id); });
    }
  }
  StartNext();
}

void Queue::StartNext() {
  const std::int64_t limit = config_.GetInt("downloads.speed_limit_mb") * 1000 * 1000;
  if (limit != applied_limit_ && aria2_.SetSpeedLimit(limit)) applied_limit_ = limit;
  int running = 0;
  for (const Download& download : downloads_) {
    if (download.state == "resolving" || download.state == "downloading") ++running;
  }
  const auto parallel = std::max<std::int64_t>(1, config_.GetInt("downloads.parallel"));
  for (Download& download : downloads_) {
    if (running >= parallel) break;
    if (download.state != "queued") continue;
    SetState(download, "resolving");
    ++running;
    work_.Post([this, id = download.id] { Fetch(id); });
  }
}

void Queue::Fetch(const std::string& id) {
  std::vector<std::string> links;
  fs::path dir;
  bool use_service = false;
  {
    std::lock_guard lock(mutex_);
    Download* download = FindLocked(id);
    if (download == nullptr || download->state != "resolving") return;
    links = download->links;
    dir = WorkDir(*download);
    use_service = download->use_service;
  }
  const std::unique_ptr<Service> service = ActiveService(config_);
  std::vector<QueuedFile> files;
  std::vector<std::string> jobs;
  Error failure;
  for (const std::string& link : links) {
    const bool service_only = Classify(link) == LinkKind::Torrent;
    if (service_only && service == nullptr) {
      failure = ServiceNeeded();
      break;
    }
    if (service != nullptr) {
      // Torrents always go through it; anything else only if it's ready there, unless direct
      // already failed.
      const auto job = service->Add(link, !service_only && !use_service);
      if (job) {
        jobs.push_back(*job);
        continue;
      }
      if (service_only || use_service) {
        failure = job.error();
        break;
      }
      if (job.error().code != kNotCached) log::Warn("{}: {}", service->Name(), job.error().message);
    }
    const auto resolved = Resolve(link);
    if (!resolved) {
      // A page Mira can't read, such as Mega: the service may still manage it.
      const auto job = service != nullptr ? service->Add(link, false)
                                          : Result<std::string>(std::unexpected(resolved.error()));
      if (job) {
        jobs.push_back(*job);
        continue;
      }
      failure = resolved.error();
      break;
    }
    for (const LinkFile& file : *resolved) {
      files.push_back(
          {.url = file.url,
           .name = file.name,
           .source = file.source,
           .gid = "",
           .status = {.state = "waiting", .total = file.size, .done = 0, .speed = 0, .error = ""}});
    }
  }
  std::error_code ec;
  if (failure.code.empty()) {
    fs::create_directories(dir, ec);
    if (ec)
      failure = {"download_failed",
                 std::format("couldn't create {}: {}", dir.string(), ec.message()), kDiskHint};
  }
  std::lock_guard lock(mutex_);
  Download* download = FindLocked(id);
  if (download == nullptr || download->state != "resolving") return;
  if (!failure.code.empty()) return SetState(*download, "failed", failure);
  download->files = std::move(files);
  if (jobs.empty()) return StartFiles(*download);
  download->jobs = std::move(jobs);
  download->service = service->Name();
  download->service_progress = 0;
  download->polling = true;
  SetState(*download, "service");
  work_.Post([this, id] { PollService(id); });
}

void Queue::StartFiles(Download& download) {
  const fs::path dir = WorkDir(download);
  for (QueuedFile& file : download.files) {
    auto gid = aria2_.Add({file.url}, dir, file.name);
    if (!gid) return SetState(download, "failed", gid.error());
    file.gid = *gid;
  }
  SetState(download, "downloading");
}

void Queue::PollService(const std::string& id) {
  std::vector<std::string> jobs;
  {
    std::lock_guard lock(mutex_);
    Download* download = FindLocked(id);
    if (download == nullptr) return;
    download->polling = false;
    if (download->state != "service") return;
    download->polling = true;
    jobs = download->jobs;
  }
  const auto finish = [&](const std::function<void(Download&)>& apply) {
    std::lock_guard lock(mutex_);
    Download* download = FindLocked(id);
    if (download == nullptr) return;
    download->polling = false;
    if (download->state == "service") apply(*download);
  };
  const auto fail = [&](Error error) {
    finish([&](Download& download) { SetState(download, "failed", std::move(error)); });
  };
  const std::unique_ptr<Service> service = ActiveService(config_);
  if (service == nullptr) return fail(ServiceNeeded());

  std::vector<QueuedFile> files;
  double progress = 0;
  bool ready = true;
  for (const std::string& job : jobs) {
    const auto status = service->Status(job);
    if (!status) {
      if (status.error().code == kServiceKeyRejected) return fail(status.error());
      // The network or the service having a bad moment: ask again next time.
      log::Warn("{}: {}", service->Name(), status.error().message);
      return finish([](Download&) {});
    }
    if (!status->failure.empty())
      return fail({"service_failed",
                   std::format("{} couldn't get it: {}", service->Name(), status->failure), ""});
    progress += status->ready ? 1.0 : status->progress;
    // Listed as ready before its files are: wait for them.
    if (!status->ready || status->files.empty()) ready = false;
    if (!ready) continue;
    for (const ServiceFile& file : status->files) {
      const auto address = service->FileAddress(job, file.id);
      if (!address) return fail(address.error());
      files.push_back(
          {.url = *address,
           .name = file.path,
           .source = service->Name(),
           .gid = "",
           .status = {.state = "waiting", .total = file.size, .done = 0, .speed = 0, .error = ""}});
    }
  }
  finish([&](Download& download) {
    if (ready) {
      download.files.insert(download.files.end(), std::make_move_iterator(files.begin()),
                            std::make_move_iterator(files.end()));
      download.jobs.clear();
      return StartFiles(download);
    }
    download.service_progress = jobs.empty() ? 0 : progress / static_cast<double>(jobs.size());
    Announce(download);
  });
}

void Queue::PollFiles(Download& download) {
  bool all_done = !download.files.empty();
  for (QueuedFile& file : download.files) {
    const auto status = aria2_.Status(file.gid);
    // aria2 restarted and lost it: add it again, continuing what's on disk.
    if (!status) {
      auto gid = aria2_.Add({file.url}, WorkDir(download), file.name);
      if (!gid) return SetState(download, "failed", gid.error());
      file.gid = *gid;
      all_done = false;
      continue;
    }
    file.status = *status;
    if (status->state == "error") {
      // The rest would keep downloading with nothing watching them. Retry continues what's on disk.
      for (QueuedFile& other : download.files) {
        if (!other.gid.empty()) aria2_.Remove(other.gid);
        other.gid.clear();
      }
      if (!download.use_service && ActiveService(config_) != nullptr) {
        log::Info("download {}: {} failed, trying the download service", download.name, file.name);
        download.use_service = true;
        download.files.clear();
        return SetState(download, "queued");
      }
      return SetState(download, "failed",
                      {"download_failed",
                       std::format("couldn't download {}: {}",
                                   file.name.empty() ? file.url : file.name, status->error),
                       ""});
    }
    if (status->state != "complete") all_done = false;
  }
  Announce(download);
  if (!all_done) return;
  for (QueuedFile& file : download.files) file.gid.clear();
  SetState(download, "extracting");
  work_.Post([this, id = download.id] { Finish(id); });
}

Result<void> Queue::Extract(const Download& download, const fs::path& dir) {
  std::vector<fs::path> entries;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().filename() != "extracted") entries.push_back(entry.path());
  }
  std::vector<fs::path> archives;
  for (const fs::path& path : entries) {
    if (library::LooksLikeArchive(path) && !library::IsLaterVolume(path)) archives.push_back(path);
  }
  const auto folders = config_.GetPathArray("mira_folders");
  if (folders.empty()) return Err("no_mira_folder", "Mira has no folder to put the game in");
  const fs::path home = folders.front();

  // Nothing to extract: an installer, a ROM or a folder of files.
  if (archives.empty()) {
    if (entries.size() == 1 && !fs::is_directory(entries.front(), ec)) {
      fs::rename(entries.front(), library::FreeName(home / entries.front().filename()), ec);
    } else {
      const fs::path dest = library::FreeName(home / FolderName(download.name));
      fs::create_directories(dest, ec);
      for (const fs::path& path : entries) fs::rename(path, dest / path.filename(), ec);
    }
    if (ec) return Err("move_failed", ec.message(), kDiskHint);
    return {};
  }

  std::vector<std::string> candidates;
  if (!download.password.empty()) candidates.push_back(download.password);
  for (std::string& saved : Passwords()) {
    if (!std::ranges::contains(candidates, saved)) candidates.push_back(std::move(saved));
  }
  std::vector<std::string> chosen;
  for (const fs::path& archive : archives) {
    const auto found = library::FindPassword(archive, candidates);
    if (!found) return std::unexpected(found.error());
    if (!*found)
      return Err("password_needed",
                 std::format("{} needs a password", archive.filename().string()));
    chosen.push_back(**found);
  }
  const fs::path extracted = dir / "extracted";
  fs::remove_all(extracted, ec);
  fs::create_directories(extracted, ec);
  if (ec) return Err("dest_dir_failed", ec.message(), kDiskHint);
  for (std::size_t i = 0; i < archives.size(); ++i) {
    if (auto done = runner::Extract(archives[i], extracted, chosen[i]); !done) return done;
    if (!chosen[i].empty()) RememberPassword(chosen[i]);
  }
  // What came alongside the archives belongs to the game too.
  for (const fs::path& path : entries) {
    if (library::LooksLikeArchive(path) || library::IsLaterVolume(path)) continue;
    const fs::path dest = extracted / path.filename();
    if (!fs::exists(dest, ec)) fs::rename(path, dest, ec);
    if (ec) return Err("move_failed", ec.message(), kDiskHint);
  }
  fs::rename(extracted, library::FreeName(home / FolderName(download.name)), ec);
  if (ec) return Err("move_failed", ec.message(), kDiskHint);

  if (config_.GetBool("downloads.keep_archives")) {
    const fs::path kept = home / "Archives" / FolderName(download.name);
    fs::create_directories(kept, ec);
    for (const fs::path& path : entries) {
      if (fs::exists(path, ec)) fs::rename(path, kept / path.filename(), ec);
    }
    if (ec) log::Warn("couldn't keep the archives of {}: {}", download.name, ec.message());
  }
  return {};
}

void Queue::Finish(const std::string& id) {
  Download copy;
  {
    std::lock_guard lock(mutex_);
    Download* download = FindLocked(id);
    if (download == nullptr || download->state != "extracting") return;
    copy = *download;
  }
  const fs::path dir = WorkDir(copy);
  const Result<void> extracted = Extract(copy, dir);
  std::lock_guard lock(mutex_);
  Download* download = FindLocked(id);
  if (download == nullptr) return;
  if (!extracted) {
    const bool needs_password = extracted.error().code == "password_needed";
    // Back to waiting; `error` says a tried password was wrong.
    if (needs_password && !download->password.empty()) {
      download->password.clear();
      return SetState(*download, "password",
                      {"wrong_password", "That password didn't open it.", ""});
    }
    return needs_password ? SetState(*download, "password")
                          : SetState(*download, "failed", extracted.error());
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
  download->password.clear();
  download->files.clear();
  SetState(*download, "done");
}

Result<nlohmann::json> Queue::Check(const std::vector<std::string>& links) const {
  json files = json::array();
  const std::unique_ptr<Service> service = ActiveService(config_);
  const auto add_file = [&files](const LinkFile& file) {
    files.push_back({{"name", file.name}, {"size", file.size}, {"source", file.source}});
  };
  for (const std::string& link : links) {
    if (Classify(link) == LinkKind::Torrent) {
      if (service == nullptr) {
        const Error error = ServiceNeeded();
        return Err(error.code, error.message, error.hint);
      }
      const std::vector<LinkFile> known = service->Peek(link);
      std::ranges::for_each(known, add_file);
      if (known.empty())
        add_file({.url = "", .name = "Torrent", .size = 0, .source = service->Name()});
      continue;
    }
    const auto resolved = Resolve(link);
    if (!resolved && service != nullptr) {
      add_file({.url = "", .name = CleanFileName("", link), .size = 0, .source = service->Name()});
      continue;
    }
    if (!resolved) return std::unexpected(resolved.error());
    for (const LinkFile& file : *resolved) {
      files.push_back({{"name", file.name}, {"size", file.size}, {"source", file.source}});
    }
  }
  return json{{"files", files}};
}

Result<nlohmann::json> Queue::Add(const std::string& name, const std::vector<std::string>& links,
                                  const std::string& password) {
  if (FolderName(name).empty()) return Err("invalid_name", "the game needs a name");
  if (links.empty()) return Err("invalid_links", "there are no links to download");
  Download download;
  download.id = NewId();
  download.name = name;
  download.links = links;
  download.state = "queued";
  download.password = password;
  std::lock_guard lock(mutex_);
  downloads_.push_back(download);
  Announce(download);
  Save();
  StartNext();
  return ToJson(downloads_.back());
}

Result<void> Queue::Pause(const std::string& id) {
  std::lock_guard lock(mutex_);
  Download* download = FindLocked(id);
  if (download == nullptr) return Err("unknown_download", "no such download");
  if (download->state == "queued") {
    SetState(*download, "paused");
    return {};
  }
  // The service keeps fetching; resuming asks it again.
  if (download->state == "service") {
    download->jobs.clear();
    SetState(*download, "paused");
    return {};
  }
  if (download->state != "downloading")
    return Err("not_running", "that download can't be paused now");
  for (QueuedFile& file : download->files) {
    if (file.status.state == "complete") continue;
    if (auto paused = aria2_.Pause(file.gid); !paused) log::Warn("{}", paused.error().message);
    file.status.speed = 0;
  }
  SetState(*download, "paused");
  return {};
}

Result<void> Queue::Resume(const std::string& id) {
  std::lock_guard lock(mutex_);
  Download* download = FindLocked(id);
  if (download == nullptr) return Err("unknown_download", "no such download");
  if (download->state != "paused") return Err("not_paused", "that download isn't paused");
  const bool live = std::ranges::any_of(download->files,
                                        [](const QueuedFile& file) { return !file.gid.empty(); });
  if (!live) {
    SetState(*download, "queued");
    StartNext();
    return {};
  }
  for (const QueuedFile& file : download->files) {
    if (file.status.state == "complete") continue;
    // One aria2 lost is added again by PollFiles.
    if (auto resumed = aria2_.Resume(file.gid); !resumed) log::Warn("{}", resumed.error().message);
  }
  SetState(*download, "downloading");
  return {};
}

Result<void> Queue::Remove(const std::string& id) {
  std::lock_guard lock(mutex_);
  const auto found = std::ranges::find(downloads_, id, &Download::id);
  if (found == downloads_.end()) return Err("unknown_download", "no such download");
  if (found->state == "extracting") return Err("download_busy", "wait for it to finish extracting");
  for (const QueuedFile& file : found->files) {
    if (!file.gid.empty()) aria2_.Remove(file.gid);
  }
  std::error_code ec;
  fs::remove_all(WorkDir(*found), ec);
  downloads_.erase(found);
  events_.Publish("download.removed", {{"id", id}});
  Save();
  return {};
}

Result<void> Queue::MoveToFront(const std::string& id) {
  std::lock_guard lock(mutex_);
  const auto found = std::ranges::find(downloads_, id, &Download::id);
  if (found == downloads_.end()) return Err("unknown_download", "no such download");
  if (found->state != "queued") return Err("not_queued", "only a queued download can move");
  const auto first = std::ranges::find(downloads_, std::string("queued"), &Download::state);
  std::rotate(first, found, found + 1);
  Save();
  return {};
}

Result<void> Queue::SubmitPassword(const std::string& id, const std::string& password) {
  if (password.empty()) return Err("invalid_password", "enter a password");
  std::lock_guard lock(mutex_);
  Download* download = FindLocked(id);
  if (download == nullptr) return Err("unknown_download", "no such download");
  if (download->state != "password")
    return Err("not_waiting", "that download isn't waiting for a password");
  download->password = password;
  SetState(*download, "extracting");
  work_.Post([this, id] { Finish(id); });
  return {};
}

Result<void> Queue::Retry(const std::string& id) {
  std::lock_guard lock(mutex_);
  Download* download = FindLocked(id);
  if (download == nullptr) return Err("unknown_download", "no such download");
  if (download->state != "failed") return Err("not_failed", "that download hasn't failed");
  download->files.clear();
  download->use_service = false;
  SetState(*download, "queued");
  StartNext();
  return {};
}

std::vector<std::string> Queue::Passwords() const {
  return ParseStrings(games_.Secret(kPasswordsKey));
}

Result<void> Queue::SetPasswords(const std::vector<std::string>& passwords) {
  return games_.SetSecret(kPasswordsKey, json(passwords).dump());
}

// Most recently working first.
void Queue::RememberPassword(const std::string& password) {
  std::vector<std::string> passwords = Passwords();
  std::erase(passwords, password);
  passwords.insert(passwords.begin(), password);
  if (auto saved = SetPasswords(passwords); !saved) log::Warn("{}", saved.error().message);
}

json Queue::List() const {
  std::lock_guard lock(mutex_);
  json out = json::array();
  for (const Download& download : downloads_) out.push_back(ToJson(download));
  return out;
}

Download* Queue::FindLocked(const std::string& id) {
  const auto found = std::ranges::find(downloads_, id, &Download::id);
  return found == downloads_.end() ? nullptr : &*found;
}

void Queue::SetState(Download& download, std::string state, Error error) {
  if (state != "downloading") {
    for (QueuedFile& file : download.files) file.status.speed = 0;
  }
  download.state = std::move(state);
  download.error = std::move(error);
  if (!download.error.code.empty())
    log::Warn("download {}: {}", download.name, download.error.message);
  Announce(download);
  Save();
}

void Queue::Announce(const Download& download) {
  events_.Publish("download.changed", ToJson(download));
}

void Queue::Save() {
  json saved = json::array();
  for (const Download& download : downloads_) {
    json item = ToJson(download);
    item.erase("files");
    item.erase("service");
    item["use_service"] = download.use_service;
    saved.push_back(std::move(item));
  }
  if (auto done = games_.SetSecret(kQueueKey, saved.dump()); !done)
    log::Warn("couldn't save the download queue: {}", done.error().message);
}

fs::path Queue::WorkDir(const Download& download) const {
  const auto folders = config_.GetPathArray("mira_folders");
  const fs::path home = folders.empty() ? paths::Home() / "Mira" : folders.front();
  return home / ".mira-downloads" / download.id;
}

}  // namespace mira::downloads
