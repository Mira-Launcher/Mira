#include <doctest.h>
#include <signal.h>

#include <string>
#include <string_view>

#include "proc/ExitReason.h"

using namespace mira;
using proc::ClassifyExit;
using proc::ExitInfo;

namespace {

ExitInfo Exit(int code, std::int64_t played = 600) {
  return {.exit_code = code,
          .signal = 0,
          .requested_stop = false,
          .launch_error = {},
          .played_seconds = played,
          .log_tail = {}};
}

ExitInfo Killed(int signal_number, std::int64_t played = 600) {
  return {.exit_code = -1,
          .signal = signal_number,
          .requested_stop = false,
          .launch_error = {},
          .played_seconds = played,
          .log_tail = {}};
}

bool Says(const std::string& error, std::string_view part) {
  return error.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("A non-zero exit on its own is an ordinary quit, however soon it comes") {
  for (const int code : {1, 3, 241}) {
    const auto outcome = ClassifyExit(Exit(code, 4));
    CHECK_FALSE(outcome.crashed);
    CHECK(outcome.error.empty());
  }
}

TEST_CASE("Crash signals are crashes, naming the cause and how long the game ran") {
  auto outcome = ClassifyExit(Killed(SIGSEGV, 250));
  CHECK(outcome.crashed);
  CHECK(outcome.code == "crashed");
  CHECK(Says(outcome.error, "4 minutes"));
  CHECK(Says(outcome.error, "segmentation fault"));

  outcome = ClassifyExit(Killed(SIGABRT, 7300));
  CHECK(Says(outcome.error, "2 hours 1 minute"));
  CHECK(Says(outcome.error, "SIGABRT"));

  SUBCASE("and so is a shell's 128 + signal for one") {
    outcome = ClassifyExit(Exit(128 + SIGSEGV));
    CHECK(outcome.crashed);
    CHECK(Says(outcome.error, "segmentation fault"));
  }
}

TEST_CASE("A SIGKILL says the game was killed, not that it crashed on its own") {
  const auto outcome = ClassifyExit(Killed(SIGKILL, 30));
  CHECK(outcome.crashed);
  CHECK(outcome.code == "killed");
  CHECK(Says(outcome.error, "memory"));
}

TEST_CASE("Being closed is an ordinary end, and anything after Stop is too") {
  CHECK_FALSE(ClassifyExit(Killed(SIGTERM)).crashed);
  CHECK_FALSE(ClassifyExit(Killed(SIGHUP)).crashed);
  ExitInfo stopped = Killed(SIGKILL);
  stopped.requested_stop = true;
  CHECK_FALSE(ClassifyExit(stopped).crashed);
  stopped = Exit(1);
  stopped.requested_stop = true;
  CHECK(ClassifyExit(stopped).error.empty());
}

TEST_CASE("A program that couldn't start says why") {
  auto outcome = ClassifyExit(Exit(127, 0));
  CHECK(outcome.code == "start_failed");
  CHECK(Says(outcome.error, "missing"));
  CHECK(Says(ClassifyExit(Exit(126, 0)).error, "executable"));

  ExitInfo wine = Exit(53, 2);
  wine.log_tail = "wine: cannot find L\"C:\\\\Games\\\\Game.exe\"\n";
  outcome = ClassifyExit(wine);
  CHECK(outcome.code == "start_failed");
  CHECK(Says(outcome.error, "Wine"));
}

TEST_CASE("Wine's unhandled-exception report names what went wrong, without its addresses") {
  ExitInfo info = Exit(5, 90);
  info.log_tail =
      "wine: Unhandled exception 0xe06d7363 in thread 24 at address 00006FFFFF4A1234 (thread "
      "0024), starting debugger...\n";
  auto outcome = ClassifyExit(info);
  CHECK(outcome.crashed);
  CHECK(Says(outcome.error, "C++ exception"));
  CHECK_FALSE(Says(outcome.error, "00006FFFFF4A1234"));

  info.log_tail =
      "0024:err:seh:NtRaiseException Unhandled exception code c0000409 flags 1 addr 0x14000\n";
  CHECK(Says(ClassifyExit(info).error, "stack buffer overrun"));

  info.log_tail =
      "wine: Unhandled page fault on read access to 0000000000000000 at address "
      "0000000140001000 (thread 0024), starting debugger...\n";
  outcome = ClassifyExit(info);
  CHECK(Says(outcome.error, "page fault"));
  CHECK_FALSE(Says(outcome.error, "0000000000000000"));
}

TEST_CASE("DescribeDuration reads like a person would say it") {
  CHECK(proc::DescribeDuration(1) == "1 second");
  CHECK(proc::DescribeDuration(59) == "59 seconds");
  CHECK(proc::DescribeDuration(60) == "1 minute");
  CHECK(proc::DescribeDuration(3600) == "1 hour");
  CHECK(proc::DescribeDuration(9000) == "2 hours 30 minutes");
}
