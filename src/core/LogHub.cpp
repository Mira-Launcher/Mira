#include "core/LogHub.h"

#include <cctype>
#include <deque>
#include <map>
#include <mutex>

namespace mira::loghub {
namespace {

constexpr std::size_t kKeepLines = 4000;

struct Channel {
  std::deque<std::string> lines;
  std::uint64_t first = 0;  // sequence number of lines.front()
  std::string partial;  // text since the last '\n' or '\r'
  std::string redraw;    // the line a '\r' cut off, which the next text replaces
  std::string progress;  // the latest download progress line, which the next one replaces
  bool active = false;
};

std::mutex g_mutex;
std::map<std::string, Channel, std::less<>>& Channels() {
  static std::map<std::string, Channel, std::less<>> channels;
  return channels;
}

Channel& Find(std::string_view name) {
  auto& channels = Channels();
  auto it = channels.find(name);
  if (it == channels.end()) it = channels.emplace(std::string(name), Channel{}).first;
  return it->second;
}

void Push(Channel& channel, std::string line) {
  channel.lines.push_back(std::move(line));
  if (channel.lines.size() > kKeepLines) {
    channel.lines.pop_front();
    ++channel.first;
  }
}

// A download's progress readout, which repeats until it is done: wget's dots ("918750K .......... 99% 39.6M 1s"),
// legendary and gogdl's "Progress: 12.3% ... ETA:", or a line that starts with a percentage.
bool IsProgress(std::string_view line) {
  const std::size_t first = line.find_first_not_of(' ');
  if (first == std::string_view::npos) return false;
  line.remove_prefix(first);
  if (line.starts_with("Progress:") || line.find(" ETA:") != std::string_view::npos) return true;
  std::size_t i = 0;
  while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
  if (i == 0) return false;
  if (i < line.size() && line[i] == '.') {
    ++i;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
  }
  if (i < line.size() && line[i] == '%') return true;  // "42% ..."
  // "918750K .......... .......... 99% 39.6M 1s"
  if (i < line.size() && (line[i] == 'K' || line[i] == 'M' || line[i] == 'G')) {
    const std::size_t dots = line.find("..........", i);
    return dots != std::string_view::npos && line.find('%', dots) != std::string_view::npos;
  }
  return false;
}

}  // namespace

void Begin(std::string_view name) {
  const std::lock_guard lock(g_mutex);
  Channel& channel = Find(name);
  channel.first += channel.lines.size();  // old cursors stay behind the new lines
  channel.lines.clear();
  channel.partial.clear();
  channel.redraw.clear();
  channel.progress.clear();
  channel.active = true;
}

void End(std::string_view name) {
  const std::lock_guard lock(g_mutex);
  Channel& channel = Find(name);
  if (!channel.progress.empty()) Push(channel, std::move(channel.progress));
  const std::string& last = channel.partial.empty() ? channel.redraw : channel.partial;
  if (!last.empty()) Push(channel, last);
  channel.partial.clear();
  channel.redraw.clear();
  channel.progress.clear();
  channel.active = false;
}

void Append(std::string_view name, std::string_view text) {
  const std::lock_guard lock(g_mutex);
  Channel& channel = Find(name);
  for (char c : text) {
    if (c == '\r') {
      // The next text replaces this line: it is the live one until a newline makes it final.
      if (!channel.partial.empty()) channel.redraw = std::move(channel.partial);
      channel.partial.clear();
    } else if (c == '\n') {
      std::string line = std::move(channel.partial.empty() ? channel.redraw : channel.partial);
      channel.partial.clear();
      channel.redraw.clear();
      if (line.empty()) continue;
      if (IsProgress(line)) {
        // Replaces the last reading instead of adding a line.
        channel.progress = std::move(line);
        continue;
      }
      // Something else follows a download: its last reading stays, as the one line it was.
      if (!channel.progress.empty()) Push(channel, std::move(channel.progress));
      channel.progress.clear();
      Push(channel, std::move(line));
    } else {
      channel.partial.push_back(c);
    }
  }
}

Page Read(std::string_view name, const std::uint64_t* after, int tail) {
  const std::lock_guard lock(g_mutex);
  Page page;
  const auto it = Channels().find(name);
  if (it == Channels().end()) return page;
  const Channel& channel = it->second;
  const std::uint64_t end = channel.first + channel.lines.size();
  std::uint64_t from = after != nullptr ? *after : (end > static_cast<std::uint64_t>(tail) ? end - tail : 0);
  from = std::max(from, channel.first);
  for (std::uint64_t i = from; i < end; ++i) page.lines.push_back(channel.lines[i - channel.first]);
  page.next = end;
  page.active = channel.active || name == "daemon";  // always being written to
  page.live = !channel.partial.empty() ? channel.partial : !channel.redraw.empty() ? channel.redraw : channel.progress;
  return page;
}

}  // namespace mira::loghub
