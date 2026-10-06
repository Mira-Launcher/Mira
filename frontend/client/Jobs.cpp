#include "Jobs.h"

#include <QCoreApplication>
#include <QPointer>
#include <QUuid>

#include <map>
#include <utility>

#include "Async.h"
#include "EventHub.h"
#include "JsonMapping.h"
#include "Transport.h"

namespace mira_gui::jobs {
namespace {

using nlohmann::json;

struct Waiter {
  QPointer<QObject> context;
  std::function<void(Outcome)> done;
  bool started = false;  // mirad accepted the request; before that it can't know the job
};

// Main thread only.
std::map<std::string, Waiter>& Pending() {
  static std::map<std::string, Waiter> pending;
  return pending;
}

void Resolve(const std::string& token, Outcome outcome) {
  const auto found = Pending().find(token);
  if (found == Pending().end()) return;
  Waiter waiter = std::move(found->second);
  Pending().erase(found);
  if (!waiter.context.isNull()) waiter.done(std::move(outcome));
}

// A job.finished / job.failed event, or GET /v1/jobs/{id}'s record once it has ended.
Outcome FromRecord(const json& record, bool failed) {
  Outcome outcome;
  outcome.ok = !failed;
  if (failed) {
    outcome.error = mapping::ToApiError(record.contains("error") ? record["error"] : json::object());
  } else {
    outcome.result = record.contains("result") ? record["result"] : json::object();
  }
  return outcome;
}

void Recheck() {
  std::erase_if(Pending(), [](const auto& entry) { return entry.second.context.isNull(); });
  for (const auto& [token, waiter] : Pending()) {
    if (!waiter.started) continue;
    Check(QCoreApplication::instance(), token, [token](Outcome outcome) { Resolve(token, std::move(outcome)); });
  }
}

void Listen() {
  static bool listening = false;
  if (listening) return;
  listening = true;
  EventHub* hub = EventHub::Instance();
  QObject::connect(hub, &EventHub::Received, hub, [](const std::string& type, const std::string& data, bool) {
    if (type != "job.finished" && type != "job.failed") return;
    const json event = json::parse(data, nullptr, false);
    if (!event.is_object()) return;
    Resolve(mapping::Str(event, "id"), FromRecord(event, type == "job.failed"));
  });
  QObject::connect(hub, &EventHub::ConnectionChanged, hub, [](bool connected) {
    if (connected) Recheck();
  });
  hub->Start();
}

}  // namespace

std::string NewToken(const std::string& kind) {
  return kind + "-" + QUuid::createUuid().toString(QUuid::Id128).toStdString();
}

void Await(QObject* context, const std::string& token, std::function<void(Outcome)> done) {
  Listen();
  Pending()[token] = {context, std::move(done)};
}

void Started(const std::string& token) {
  if (const auto found = Pending().find(token); found != Pending().end()) found->second.started = true;
}

void Check(QObject* context, const std::string& token, std::function<void(Outcome)> ended) {
  async::Run(context, [token] { return transport::Get("/v1/jobs/" + token); },
             std::function<void(transport::Reply)>([token, ended](transport::Reply reply) {
               if (reply.status == 404) {
                 Outcome lost;
                 lost.error = ApiError("mirad restarted before this finished.");
                 ended(std::move(lost));
                 return;
               }
               if (!reply.ok || !reply.body.is_object()) return;  // still unreachable: the next reconnect asks again
               const std::string state = mapping::Str(reply.body, "state");
               if (state == "finished" || state == "failed") ended(FromRecord(reply.body, state == "failed"));
             }));
}

bool Forget(const std::string& token) { return Pending().erase(token) > 0; }

void Cancel(QObject* context, const std::string& token, std::function<void(ApiError)> done) {
  async::Run(context, [token] { return transport::Post("/v1/jobs/" + token + "/cancel"); },
             std::function<void(transport::Reply)>([done](transport::Reply reply) {
               // Already over: nothing left to cancel.
               done(reply.ok || reply.status == 409 ? ApiError() : reply.error);
             }));
}

}  // namespace mira_gui::jobs
