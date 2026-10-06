#include "Logs.h"

#include <json.hpp>

#include "../Async.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
namespace {

using nlohmann::json;

LogResult GetLogSync(const std::string& channel, const std::optional<std::uint64_t>& after, int lines) {
  std::string path = "/v1/logs/" + channel + "?lines=" + std::to_string(lines);
  if (after) path += "&after=" + std::to_string(*after);
  return ReadReply<LogResult>(transport::Get(path), "GET /v1/logs/" + channel, Shape::Object,
                              [](LogResult& result, const json& body) {
                                if (body.contains("lines") && body["lines"].is_array()) {
                                  for (const json& line : body["lines"]) {
                                    if (line.is_string()) result.lines.push_back(line.get<std::string>());
                                  }
                                }
                                result.next = body.value("next", std::uint64_t{0});
                                result.active = body.value("active", false);
                                result.live = body.value("live", std::string());
                              });
}

}  // namespace

void GetLogAsync(QObject* context, const std::string& channel, const std::optional<std::uint64_t>& after,
                 int lines, std::function<void(LogResult)> callback) {
  async::Run(context, [channel, after, lines] { return GetLogSync(channel, after, lines); }, std::move(callback));
}

}  // namespace mira_gui::api
