#include "core/LogHub.h"

#include <deque>
#include <map>
#include <mutex>

namespace mira::loghub {
namespace {

constexpr std::size_t kKeepLines = 4000;

struct Channel {
  std::deque<std::string> lines;
  std::uint64_t first = 0;  // sequence number of lines.front()
  std::string partial;
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

}  // namespace

void Begin(std::string_view name) {
  const std::lock_guard lock(g_mutex);
  Channel& channel = Find(name);
  channel.first += channel.lines.size();  // old cursors stay behind the new lines
  channel.lines.clear();
  channel.partial.clear();
  channel.active = true;
}

void End(std::string_view name) {
  const std::lock_guard lock(g_mutex);
  Channel& channel = Find(name);
  if (!channel.partial.empty()) {
    Push(channel, std::move(channel.partial));
    channel.partial.clear();
  }
  channel.active = false;
}

void Append(std::string_view name, std::string_view text) {
  const std::lock_guard lock(g_mutex);
  Channel& channel = Find(name);
  for (char c : text) {
    if (c != '\n' && c != '\r') {
      channel.partial.push_back(c);
      continue;
    }
    if (channel.partial.empty()) continue;
    Push(channel, std::move(channel.partial));
    channel.partial.clear();
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
  return page;
}

}  // namespace mira::loghub
