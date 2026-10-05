#include <doctest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "api/EventBus.h"
#include "api/Jobs.h"

using namespace mira;

TEST_CASE("WaitNext delivers an event published concurrently, with no polling") {
  // This is the exact shape the SSE handler and the API thread run in
  // production: one thread blocked in WaitNext, another calling Publish.
  // Run under the tsan preset, this is what actually exercises the
  // condition-variable handoff that Server.cpp relies on.
  api::EventBus bus;
  std::atomic<bool> stop{false};
  std::optional<model::Event> received;

  std::thread subscriber([&] { received = bus.WaitNext(0, stop, std::chrono::seconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let it start waiting
  bus.Publish("game.added", {{"id", "celeste"}});
  subscriber.join();

  REQUIRE(received.has_value());
  CHECK(received->type == "game.added");
  CHECK(received->payload.value("id", "") == "celeste");
}

TEST_CASE("WaitNext times out cleanly with no event and no stop") {
  api::EventBus bus;
  std::atomic<bool> stop{false};
  auto result = bus.WaitNext(0, stop, std::chrono::milliseconds(50));
  CHECK_FALSE(result.has_value());
}

TEST_CASE("WaitNext unblocks immediately when stop is set, before or during the wait") {
  api::EventBus bus;
  std::atomic<bool> stop{true};
  auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(bus.WaitNext(0, stop, std::chrono::seconds(30)).has_value());
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));

  // Shutdown's shape: stop is set while a stream is already waiting.
  stop = false;
  start = std::chrono::steady_clock::now();
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop = true;
    bus.WakeWaiters();
  });
  CHECK_FALSE(bus.WaitNext(0, stop, std::chrono::seconds(30)).has_value());
  stopper.join();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}

TEST_CASE("many publishers and many subscribers race safely") {
  // Not a correctness oracle for ordering (only the ring buffer's own mutex
  // guarantees that), and this exists to give TSan a genuinely concurrent
  // workload against Publish/WaitNext/Since together.
  api::EventBus bus;
  std::atomic<bool> stop{false};
  std::atomic<int> delivered{0};

  std::vector<std::thread> subscribers;
  for (int i = 0; i < 4; ++i) {
    subscribers.emplace_back([&] {
      std::int64_t after = 0;
      while (!stop.load()) {
        if (auto event = bus.WaitNext(after, stop, std::chrono::milliseconds(100))) {
          after = event->id;
          delivered.fetch_add(1);
        }
      }
    });
  }

  std::vector<std::thread> publishers;
  for (int i = 0; i < 4; ++i) {
    publishers.emplace_back([&, i] {
      for (int n = 0; n < 25; ++n) bus.Publish("stress", {{"from", i}, {"n", n}});
    });
  }
  for (auto& t : publishers) t.join();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true);
  for (auto& t : subscribers) t.join();

  const auto all = bus.Since(0);
  REQUIRE(all.size() == 100);
  CHECK(all.back().id - all.front().id == 99);
  CHECK(delivered.load() > 0);
}

TEST_CASE("Every game record an event carries gets the record hook, and a state change its own running") {
  api::EventBus bus;
  bus.SetGameRecordHook([](nlohmann::json& game) { game["running"] = game.value("id", "") == "celeste"; });

  CHECK(bus.Publish("game.updated", {{"id", "celeste"}}).payload.value("running", false));
  CHECK_FALSE(bus.Publish("game.added", {{"id", "hades"}}).payload.value("running", true));
  const auto many = bus.Publish("games.updated", {{"games", {{{"id", "celeste"}}, {{"id", "hades"}}}}}).payload;
  CHECK(many["games"][0].value("running", false));
  CHECK_FALSE(many["games"][1].value("running", true));
  // The state says it, whatever the hook would.
  CHECK_FALSE(bus.Publish("game.state", {{"id", "celeste"}, {"state", "exited"}}).payload.value("running", true));
  CHECK(bus.Publish("game.state", {{"id", "hades"}, {"state", "running"}}).payload.value("running", false));
  // Not a game record.
  CHECK_FALSE(bus.Publish("game.removed", {{"id", "celeste"}}).payload.contains("running"));
}

TEST_CASE("Art events carry the game's current art") {
  api::EventBus bus;
  bus.SetArtHook([](const std::string& id) { return nlohmann::json{{"cover", id + "-v2"}}; });

  for (const char* type : {"game.metadata_ready", "game.metadata_failed", "game.artwork_selected"}) {
    CHECK(bus.Publish(type, {{"id", "celeste"}}).payload["art"]["cover"] == "celeste-v2");
  }
  CHECK_FALSE(bus.Publish("game.removed", {{"id", "celeste"}}).payload.contains("art"));
}

TEST_CASE("A job that throws still fails, and a running job outlives the cap on kept jobs") {
  api::EventBus bus;
  std::atomic<bool> release{false};
  std::string thrower;
  std::string slow;
  {
    api::JobRegistry jobs(bus);
    thrower = jobs.Start("scan", "", "Scanning", [](auto&) -> Result<nlohmann::json> {
      throw std::runtime_error("boom");
    });
    slow = jobs.Start("import", "", "Importing", [&release](auto&) -> Result<nlohmann::json> {
      while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return nlohmann::json::object();
    });
    for (int i = 0; i < 150; ++i) {
      jobs.Start("scan", "", "Scanning", [](auto&) -> Result<nlohmann::json> { return nlohmann::json::object(); });
    }
    CHECK(jobs.Find(slow).has_value());
    release = true;
  }
  bool failed = false;
  for (const model::Event& event : bus.Since(0)) {
    if (event.type == "job.failed" && event.payload.value("id", "") == thrower) {
      failed = event.payload["error"].value("code", "") == "internal_error";
    }
  }
  CHECK(failed);
}
