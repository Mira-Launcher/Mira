#include "api/Jobs.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>

#include "api/EventBus.h"
#include "runner/Exec.h"
#include "support/TestEnv.h"

using namespace mira;

TEST_CASE("Cancelling a job kills what it runs and ends it as cancelled") {
  api::EventBus events;
  api::JobRegistry jobs(events);
  nlohmann::json failure;
  const std::filesystem::path started_file = test::TempDir("jobs-cancel") / "started";
  const std::string id = jobs.Start(
      "install", "gog-1", "Installing", [&](api::JobRegistry::Progress&) -> Result<nlohmann::json> {
        Command command;
        command.argv = {"sh", "-c", "touch \"$0\"; exec sleep 30", started_file.string()};
        auto ran = runner::RunAndWait(command);
        failure = api::FailedEvent({{"source", "gog"}}, ran ? Error{} : ran.error());
        if (!ran) return std::unexpected(ran.error());
        return nlohmann::json::object();
      });
  REQUIRE(test::WaitUntil([&] { return std::filesystem::exists(started_file); }));

  // Left to run, the job would take its whole 30 s.
  const auto started = std::chrono::steady_clock::now();
  REQUIRE(jobs.Cancel(id).has_value());
  CHECK(test::WaitUntil([&] { return jobs.Find(id)->value("state", "") != "running"; }));
  CHECK(jobs.Find(id)->value("state", "") == "failed");
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(15));
  CHECK(jobs.Find(id)->at("error").value("code", "") == "cancelled");
  CHECK(failure.value("code", "") == "cancelled");  // the work's own *.failed event says so too

  CHECK(jobs.Cancel(id).error().code == "not_running");
  CHECK(jobs.Cancel("no-such-job").error().code == "job_not_found");
}
