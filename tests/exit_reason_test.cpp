#include <doctest.h>

#include <signal.h>

#include "proc/ExitReason.h"

using namespace mira;
using proc::ClassifyExit;
using proc::ExitInfo;

namespace {

ExitInfo Exit(int code, std::int64_t played = 600) {
  return {.exit_code = code, .signal = 0, .requested_stop = false, .launch_error = {}, .played_seconds = played, .log_tail = {}};
}

ExitInfo Killed(int signal_number, std::int64_t played = 600) {
  return {.exit_code = -1, .signal = signal_number, .requested_stop = false, .launch_error = {}, .played_seconds = played, .log_tail = {}};
}

}  // namespace

TEST_CASE("A non-zero exit on its own is an ordinary quit, however soon it comes") {
  for (const int code : {1, 3, 241}) {
    const auto outcome = ClassifyExit(Exit(code, 4));
    CHECK_FALSE(outcome.crashed);
    CHECK(outcome.error.empty());
  }
}

TEST_CASE("Crash signals are crashes, said in plain words") {
  auto outcome = ClassifyExit(Killed(SIGSEGV, 250));
  CHECK(outcome.crashed);
  CHECK(outcome.code == "crashed");
  CHECK(outcome.error == "Crashed after 4 minutes: invalid memory access (segmentation fault)");

  outcome = ClassifyExit(Killed(SIGABRT, 7300));
  CHECK(outcome.error == "Crashed after 2 hours 1 minute: the game aborted itself (SIGABRT)");

  SUBCASE("and so is a shell's 128 + signal for one") {
    outcome = ClassifyExit(Exit(128 + SIGSEGV));
    CHECK(outcome.crashed);
    CHECK(outcome.error.find("segmentation fault") != std::string::npos);
  }
}

TEST_CASE("A SIGKILL says the game was killed, not that it crashed on its own") {
  const auto outcome = ClassifyExit(Killed(SIGKILL, 30));
  CHECK(outcome.crashed);
  CHECK(outcome.code == "killed");
  CHECK(outcome.error == "Killed after 30 seconds, often because the system ran out of memory");
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
  CHECK(outcome.error == "Couldn't start: the program or a library it needs is missing");
  CHECK(ClassifyExit(Exit(126, 0)).error == "Couldn't start: the program isn't executable");

  ExitInfo wine = Exit(53, 2);
  wine.log_tail = "wine: cannot find L\"C:\\\\Games\\\\Game.exe\"\n";
  CHECK(ClassifyExit(wine).error == "Couldn't start: Wine couldn't find or load the program");
}

TEST_CASE("Wine's unhandled-exception report names what went wrong") {
  ExitInfo info = Exit(5, 90);
  info.log_tail = "wine: Unhandled exception 0xe06d7363 in thread 24 at address 00006FFFFF4A1234 (thread 0024), "
                  "starting debugger...\n";
  CHECK(ClassifyExit(info).error == "Crashed after 1 minute with an unhandled C++ exception");

  info.log_tail = "0024:err:seh:NtRaiseException Unhandled exception code c0000409 flags 1 addr 0x14000\n";
  CHECK(ClassifyExit(info).error == "Crashed after 1 minute with an unhandled stack buffer overrun");

  info.log_tail = "wine: Unhandled illegal instruction at address 0000000140001000 (thread 0024), starting debugger...\n";
  CHECK(ClassifyExit(info).error == "Crashed after 1 minute with an unhandled illegal instruction");
}

TEST_CASE("DescribeDuration reads like a person would say it") {
  CHECK(proc::DescribeDuration(1) == "1 second");
  CHECK(proc::DescribeDuration(59) == "59 seconds");
  CHECK(proc::DescribeDuration(60) == "1 minute");
  CHECK(proc::DescribeDuration(3600) == "1 hour");
  CHECK(proc::DescribeDuration(9000) == "2 hours 30 minutes");
}
