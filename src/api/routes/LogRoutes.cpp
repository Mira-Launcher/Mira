#include "api/Routes.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "core/LogHub.h"
#include "core/Strings.h"
#include "proc/ProcessSupervisor.h"
#include "proc/Session.h"

namespace mira::api {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::streamoff kMaxReadBytes = 4 * 1024 * 1024;

// A game's log is mira-run's file; the cursor is a byte offset into it.
json GameLog(Services& s, const std::string& id, const std::optional<std::uint64_t>& after, int tail) {
  std::ifstream in(proc::GameLogPath(s.games.Dir(), id), std::ios::binary);
  const bool active = s.supervisor.IsRunning(id);
  if (!in) return {{"lines", json::array()}, {"next", 0}, {"active", active}};
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  // No cursor, or one from a longer file that was since replaced: start from the end.
  const bool from_end = !after || static_cast<std::streamoff>(*after) > size;
  std::streamoff start = from_end ? std::max<std::streamoff>(0, size - kMaxReadBytes) : static_cast<std::streamoff>(*after);
  start = std::max(start, size - kMaxReadBytes);
  in.seekg(start);
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> lines = strings::Split(content, '\n');
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  // A progress bar redrawn with '\r' is its last drawing, not every one.
  for (std::string& line : lines) {
    if (const std::size_t cr = line.find_last_of('\r'); cr != std::string::npos) {
      const std::size_t start = line.find_last_of('\r', cr == 0 ? 0 : cr - 1);
      line = cr + 1 < line.size() ? line.substr(cr + 1) : (start == std::string::npos ? line.substr(0, cr) : line.substr(start + 1, cr - start - 1));
    }
  }
  if (from_end && lines.size() > static_cast<std::size_t>(tail)) lines.erase(lines.begin(), lines.end() - tail);
  return {{"lines", lines}, {"next", size}, {"active", active}};
}

}  // namespace

// GET /v1/logs/<channel>?after=<cursor>&lines=<n>: a live log. Channels:
// daemon, game:<id>, setup:<source>, install:<source>:<ref>, runner:<kind>:<name>.
// Each task has its own, so two running at once don't mix. `next` is the cursor to send back.
void RegisterLogRoutes(httplib::Server& http, Services& s) {
  http.Get(R"(/v1/logs/([A-Za-z0-9._:-]+))", [&s](const httplib::Request& req, httplib::Response& res) {
    const std::string channel = req.matches[1];
    int tail = 300;
    if (req.has_param("lines")) {
      const std::string raw = req.get_param_value("lines");
      if (std::from_chars(raw.data(), raw.data() + raw.size(), tail).ec != std::errc() || tail < 1) {
        return SendError(res, 400, "invalid_param", "?lines= must be a whole number, 1 or more");
      }
    }
    std::optional<std::uint64_t> after;
    if (req.has_param("after")) {
      const std::string raw = req.get_param_value("after");
      std::uint64_t value = 0;
      if (std::from_chars(raw.data(), raw.data() + raw.size(), value).ec != std::errc()) {
        return SendError(res, 400, "invalid_param", "?after= must be a whole number");
      }
      after = value;
    }
    if (channel.starts_with("game:")) {
      const std::string id = channel.substr(5);
      if (!s.games.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
      return SendJson(res, GameLog(s, id, after, tail));
    }
    const loghub::Page page = loghub::Read(channel, after ? &*after : nullptr, tail);
    SendJson(res, {{"lines", page.lines}, {"next", page.next}, {"active", page.active}, {"live", page.live}});
  });
}

}  // namespace mira::api
