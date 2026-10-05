#include <doctest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <chrono>
#include <thread>

#include "core/Lane.h"

using namespace mira;

TEST_CASE("a lane runs tasks in order on one worker and never exceeds its worker count") {
  Lane one("test-one", 1);
  std::string order;
  for (char c : std::string("abc")) one.Post([&order, c] { order += c; });
  one.WaitIdle();
  CHECK(order == "abc");

  Lane two("test-two", 2);
  std::atomic<int> active = 0;
  std::atomic<int> peak = 0;
  for (int i = 0; i < 8; ++i) {
    two.Post([&] {
      const int now = ++active;
      int seen = peak.load();
      while (now > seen && !peak.compare_exchange_weak(seen, now)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      --active;
    });
  }
  two.WaitIdle();
  CHECK(peak <= 2);
}

TEST_CASE("stopping a lane signals the running task, drops the queue and survives a throwing task") {
  std::atomic<bool> saw_stop = false;
  std::atomic<bool> started = false;
  std::atomic<int> ran_after = 0;
  {
    Lane lane("test-stop", 1);
    lane.Post([] { throw std::runtime_error("boom"); });
    lane.Post([&] {
      started = true;
      while (!ThisTaskStop().stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      saw_stop = true;
    });
    lane.Post([&] { ++ran_after; });
    while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(saw_stop);
  CHECK(ran_after == 0);
}
