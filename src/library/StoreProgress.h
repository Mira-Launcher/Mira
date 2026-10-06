#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "api/EventBus.h"

namespace mira::library {

struct DownloadProgress {
  double fraction = 0;          // 0..1
  std::int64_t eta_seconds = -1;  // -1: not reported
  double bytes_per_second = -1;
};

// Reads one line of legendary, gogdl or nile download output. They share
// Legendary's log format: "Progress: 12.34% (...), ETA: 00:01:20" and
// "+ Download - 12.34 MiB/s". Updates only what the line reports; true if it
// reported progress.
bool ParseProgressLine(std::string_view line, DownloadProgress& progress);

// Turns a store tool's output into library.install.progress events, at most
// one a second.
class StoreProgress {
public:
  StoreProgress(api::EventBus& events, std::string source, std::string ref);
  ~StoreProgress();
  StoreProgress(const StoreProgress&) = delete;
  StoreProgress& operator=(const StoreProgress&) = delete;

  void Feed(std::string_view chunk);

private:
  // The log of this install: "install:<source>:<ref>".
  std::string channel() const { return "install:" + source_ + ":" + ref_; }

  api::EventBus& events_;
  std::string source_;
  std::string ref_;
  std::string partial_;  // a line still being written
  DownloadProgress progress_;
  std::chrono::steady_clock::time_point last_sent_{};
};

}  // namespace mira::library
