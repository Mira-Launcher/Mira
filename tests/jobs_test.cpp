#include <doctest.h>

#include <chrono>
#include <thread>

#include "api/EventBus.h"
#include "api/Jobs.h"
#include "runner/Exec.h"

using namespace mira;

namespace {

std::optional<std::string> WaitForState(const api::JobRegistry& jobs, const std::string& id) {
  for (int tries = 0; tries < 100; ++tries) {
    const auto job = jobs.Find(id);
    if (job && job->value("state", "") != "running") return job->value("state", "");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("Cancelling a job kills what it runs and ends it as cancelled") {
  api::EventBus events;
  api::JobRegistry jobs(events);
  nlohmann::json failure;
  const std::string id = jobs.Start("install", "gog-1", "Installing", [&](api::JobRegistry::Progress&) -> Result<nlohmann::json> {
    Command command;
    command.argv = {"sleep", "30"};
    auto ran = runner::RunAndWait(command);
    failure = api::FailedEvent({{"source", "gog"}}, ran ? Error{} : ran.error());
    if (!ran) return std::unexpected(ran.error());
    return nlohmann::json::object();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto started = std::chrono::steady_clock::now();
  REQUIRE(jobs.Cancel(id).has_value());
  CHECK(WaitForState(jobs, id) == "failed");
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
  CHECK(jobs.Find(id)->at("error").value("code", "") == "cancelled");
  CHECK(failure.value("code", "") == "cancelled");  // the work's own *.failed event says so too

  CHECK(jobs.Cancel(id).error().code == "not_running");
  CHECK(jobs.Cancel("no-such-job").error().code == "job_not_found");
}
