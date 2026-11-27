#include "epic/Playtime.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <random>

#include "achievements/Achievements.h"
#include "core/Json.h"
#include "core/Log.h"
#include "epic/Legendary.h"
#include "runner/Exec.h"

namespace mira::epic {
namespace {
using nlohmann::json;

// Taking Epic's total and sending sessions both move `seen`, so they never interleave.
std::mutex& SyncMutex() {
  static std::mutex mutex;
  return mutex;
}

struct Session {
  std::string account;
  std::string token;
};

std::string Utc(std::int64_t unix_seconds, bool millis) {
  const auto at = std::chrono::sys_seconds(std::chrono::seconds(unix_seconds));
  return millis ? std::format("{:%FT%T}.000Z", at) : std::format("{:%FT%T}", at);
}

// legendary's signed-in session, renewed through legendary when its token expired.
std::optional<Session> SignedIn(const config::Config& config) {
  const auto read = [] {
    std::ifstream file(LegendaryConfigDir() / "user.json");
    const json user = json::parse(file, nullptr, false);
    return user.is_object() ? user : json::object();
  };
  json user = read();
  if (core::JsonString(user, "access_token").empty()) return std::nullopt;
  // ISO 8601 in UTC, so the text order is the time order. Any command that signs in renews it.
  const std::string now = Utc(model::NowSeconds(), false);
  if (const std::string expires = core::JsonString(user, "expires_at");
      !expires.empty() && expires <= now) {
    if (auto renewed = RunLegendaryJson(config, {"status", "--json"}); !renewed) {
      log::Warn("couldn't renew Epic's session for its playtime: {}", renewed.error().message);
      return std::nullopt;
    }
    user = read();
  }
  Session session{core::JsonString(user, "account_id"), core::JsonString(user, "access_token")};
  if (session.account.empty() || session.token.empty()) return std::nullopt;
  return session;
}

std::string PlaytimeUrl(const Session& session, std::string_view what) {
  return std::format(
      "{}/library/api/public/playtime/account/{}{}",
      achievements::Endpoint("EPIC_LIBRARY", "https://library-service.live.use1a.on.epicgames.com"),
      session.account, what);
}

std::string Bearer(const Session& session) {
  return runner::CurlConfigLine("header", "Authorization: Bearer " + session.token);
}

// One id for this mirad, as the Epic Games Launcher sends one per machine.
const std::string& MachineId() {
  static const std::string id = [] {
    std::random_device device;
    std::mt19937_64 random(device());
    const std::uint64_t high = random(), low = random();
    return std::format("{:08x}-{:04x}-4{:03x}-{:04x}-{:012x}", high >> 32, (high >> 16) & 0xffff,
                       high & 0xfff, (low >> 48 & 0x3fff) | 0x8000, low & 0xffffffffffff);
  }();
  return id;
}

}  // namespace

std::optional<std::map<std::string, std::int64_t>> Playtime(const config::Config& config) {
  const auto session = SignedIn(config);
  if (!session) return std::nullopt;
  const auto answer = runner::RunCurlWithSecrets(
      {"-sSfL", "--max-time", "15", PlaytimeUrl(*session, "/all")}, Bearer(*session));
  const json items = answer ? json::parse(answer->output, nullptr, false) : json();
  if (!items.is_array()) {
    log::Warn("Epic's playtime wasn't readable{}",
              answer ? "" : std::format(": {}", answer.error().message));
    return std::nullopt;
  }
  std::map<std::string, std::int64_t> seconds;
  for (const json& item : items) {
    const std::string app = core::JsonString(item, "artifactId");
    const auto total = item.is_object() ? item.find("totalTime") : item.end();
    if (!app.empty() && total != item.end() && total->is_number_integer())
      seconds[app] = total->get<std::int64_t>();
  }
  return seconds;
}

void TakePlaytime(store::GameStore& games, const std::string& id, std::int64_t epic_total) {
  const std::lock_guard lock(SyncMutex());
  const auto last = games.StorePlaytimeOf(id);
  store::StorePlaytime next{.seen = epic_total,
                            .sent_through = last ? last->sent_through : model::NowSeconds()};
  const auto updated = games.Update(id, [&](model::Game& game) {
    if (!last)
      game.play_seconds = std::max(game.play_seconds, epic_total);
    else if (epic_total > last->seen)
      game.play_seconds += epic_total - last->seen;
  });
  if (!updated)
    return log::Warn("couldn't add Epic's playtime to {}: {}", id, updated.error().message);
  if (auto saved = games.SetStorePlaytime(id, next); !saved)
    log::Warn("couldn't keep Epic's playtime for {}: {}", id, saved.error().message);
}

void SendPlaytime(const config::Config& config, store::GameStore& games, const std::string& id) {
  const std::lock_guard lock(SyncMutex());
  auto state = games.StorePlaytimeOf(id);
  const auto game = games.Find(id);
  if (!state || !game || game->source != "epic" || game->source_ref.empty()) return;
  std::vector<store::PlaySession> unsent;
  for (const store::PlaySession& session : games.Sessions(id, 100))
    if (session.ended_at > state->sent_through && session.duration_seconds > 0)
      unsent.push_back(session);
  if (unsent.empty()) return;
  const auto session = SignedIn(config);
  if (!session) return;

  // Minute-long pieces, as the Epic Games Launcher reports a game while it runs: Epic doesn't
  // count one interval much longer than that. Each session's played seconds end at its end.
  constexpr std::int64_t kPiece = 60;
  json pieces = json::array();
  std::int64_t sent = 0, through = state->sent_through;
  for (const store::PlaySession& played : unsent) {
    const std::int64_t start = played.ended_at - played.duration_seconds;
    for (std::int64_t at = start; at < played.ended_at; at += kPiece) {
      pieces.push_back({{"machineId", MachineId()},
                        {"artifactId", game->source_ref},
                        {"startTime", Utc(at, true)},
                        {"endTime", Utc(std::min(at + kPiece, played.ended_at), true)},
                        {"startSegment", true},
                        {"endSegment", true}});
    }
    sent += played.duration_seconds;
    through = std::max(through, played.ended_at);
  }
  const auto answer = runner::RunCurlWithSecrets(
      {"-sSfL", "--max-time", "30", "-X", "PUT", "-H", "Content-Type: application/json",
       "--data-binary", json{{"playtimeAddList", pieces}}.dump(), PlaytimeUrl(*session, "/bulk")},
      Bearer(*session));
  if (!answer || answer->exit_code != 0) {
    log::Warn("couldn't send {}'s playtime to Epic{}", id,
              answer ? "" : std::format(": {}", answer.error().message));
    return;
  }
  // Epic's total now holds these, so the next TakePlaytime doesn't add them again.
  state->seen += sent;
  state->sent_through = through;
  if (auto saved = games.SetStorePlaytime(id, *state); !saved)
    log::Warn("couldn't keep what was sent to Epic for {}: {}", id, saved.error().message);
}

}  // namespace mira::epic
