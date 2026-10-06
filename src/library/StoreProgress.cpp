#include "library/StoreProgress.h"

#include <algorithm>
#include <regex>

#include "core/LogHub.h"

namespace mira::library {

bool ParseProgressLine(std::string_view line, DownloadProgress& progress) {
  static const std::regex kProgress(R"(Progress:\s*([0-9]+(?:\.[0-9]+)?)\s*%?)");
  static const std::regex kEta(R"(ETA:\s*([0-9]+):([0-9]{2}):([0-9]{2}))");
  static const std::regex kSpeed(R"(Download\s*-\s*([0-9]+(?:\.[0-9]+)?)\s*MiB/s)");
  std::cmatch match;
  const char* begin = line.data();
  const char* end = line.data() + line.size();
  if (std::regex_search(begin, end, match, kSpeed)) {
    progress.bytes_per_second = std::stod(match[1].str()) * 1024 * 1024;
  }
  if (!std::regex_search(begin, end, match, kProgress)) return false;
  progress.fraction = std::min(1.0, std::stod(match[1].str()) / 100.0);
  if (std::regex_search(begin, end, match, kEta)) {
    progress.eta_seconds =
        std::stoll(match[1].str()) * 3600 + std::stoll(match[2].str()) * 60 + std::stoll(match[3].str());
  }
  return true;
}

StoreProgress::StoreProgress(api::EventBus& events, std::string source, std::string ref)
    : events_(events), source_(std::move(source)), ref_(std::move(ref)) {
  loghub::Begin(channel());
}

StoreProgress::~StoreProgress() { loghub::End(channel()); }

void StoreProgress::Feed(std::string_view chunk) {
  loghub::Append(channel(), chunk);
  bool changed = false;
  for (char c : chunk) {
    // Progress bars redraw with '\r'; both end a line here.
    if (c != '\n' && c != '\r') {
      partial_.push_back(c);
      continue;
    }
    if (ParseProgressLine(partial_, progress_)) changed = true;
    partial_.clear();
  }
  const auto now = std::chrono::steady_clock::now();
  if (!changed || now - last_sent_ < std::chrono::seconds(1)) return;
  last_sent_ = now;
  events_.Publish("library.install.progress", {{"source", source_},
                                               {"ref", ref_},
                                               {"progress", progress_.fraction},
                                               {"eta", progress_.eta_seconds},
                                               {"bps", progress_.bytes_per_second}});
}

}  // namespace mira::library
