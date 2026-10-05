#pragma once

#include <QObject>
#include <exception>
#include <functional>
#include <json.hpp>
#include <string>
#include <utility>

#include "../Async.h"
#include "../Jobs.h"
#include "../Transport.h"

// Request helpers the api/ areas share.
namespace mira_gui::api {

// Percent-encodes everything outside RFC 3986's unreserved set, for a path segment or a query
// value.
std::string PercentEncode(const std::string& text);

// Starts a job (docs/api.md#jobs) and hands its outcome to `callback` as an
// R, with `fill` reading the job's result. `start` sends the request with
// `query` ("?job=<token>") appended, so the waiter is listening before the 202.
template <typename R>
void RunJob(QObject* context, const std::string& kind,
            std::function<transport::Reply(const std::string&)> start,
            std::function<void(R&, const nlohmann::json&)> fill, std::function<void(R)> callback) {
  const std::string token = jobs::NewToken(kind);
  jobs::Await(context, token, [fill, callback](jobs::Outcome outcome) {
    R result;
    if (!outcome.ok) {
      result.error = outcome.error;
    } else {
      try {
        fill(result, outcome.result);
        result.ok = true;
      } catch (const std::exception& e) {
        result.error = e.what();
      }
    }
    callback(std::move(result));
  });
  async::Run(
      context, [start, token] { return start("?job=" + token); },
      std::function<void(transport::Reply)>([token, callback](transport::Reply reply) {
        if (reply.ok) {
          jobs::Started(token);
          return;
        }
        // Refused before it started (a bad body, an unknown source, mirad down).
        if (!jobs::Forget(token)) return;
        R result;
        result.error = reply.error;
        callback(std::move(result));
      }));
}

// Steam scans, store and launcher imports all answer {added, updated}.
template <typename R>
void FillAddedUpdated(R& result, const nlohmann::json& body) {
  result.added = body.value("added", 0);
  result.updated = body.value("updated", 0);
}

}  // namespace mira_gui::api
