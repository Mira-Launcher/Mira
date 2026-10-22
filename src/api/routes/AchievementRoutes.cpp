#include <httplib.h>

#include <chrono>
#include <fstream>
#include <future>
#include <memory>

#include "achievements/Achievements.h"
#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "metadata/ImageFit.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

// The game behind a request, answering 404 or 400 itself when there's none to show.
std::optional<model::Game> AchievementsGame(Services& s, const Request& req, Response& res) {
  auto game = s.games.Find(req.matches[1]);
  if (!game) {
    SendError(res, 404, "game_not_found", "no such game");
    return std::nullopt;
  }
  if (!achievements::HasSource(*game)) {
    SendError(res, 400, "no_achievements_source",
              "achievements are shown for Steam, GOG and Epic games only");
    return std::nullopt;
  }
  // Turned off, they aren't shown; what's cached stays for when it's turned on again.
  if (!s.config.GetBool("achievements.enabled")) {
    SendError(
        res, 400,
        Error{"achievements_off", "achievements are turned off",
              "Turn on Show achievements to check them.", Fix::Setting("achievements.enabled")});
    return std::nullopt;
  }
  return game;
}

}  // namespace

void RegisterAchievementRoutes(Router& http, Services& s) {
  http.Get(R"(/v1/games/([^/]+)/achievements)", [&s](const Request& req, Response& res) {
    const auto game = AchievementsGame(s, req, res);
    if (!game) return;
    const json cached = s.games.Metadata().ReadAchievements(game->id);
    if (cached.is_null())
      return SendError(res, 404, "no_achievements", "no achievements cached for this game yet");
    SendJson(res, achievements::ApiJson(achievements::FromJson(cached)));
  });

  http.Post(R"(/v1/games/([^/]+)/achievements/refresh)", [&s](const Request& req, Response& res) {
    const auto game = AchievementsGame(s, req, res);
    if (!game) return;
    s.StartJob(
        req, res, "achievements", game->id, "Checking achievements for " + game->name,
        [&s, id = game->id](JobRegistry::Progress&) { return s.RefreshAchievements(id); },
        &s.achievements);
  });

  // Downloaded the first time a client asks, a few at a time.
  http.Get(R"(/v1/games/([^/]+)/achievements/icon)", [&s](const Request& req, Response& res) {
    const auto game = AchievementsGame(s, req, res);
    if (!game) return;
    const std::string key = Param(req, "key");
    const std::string state = Param(req, "state", "unlocked");
    if (key.empty() || (state != "locked" && state != "unlocked"))
      return SendError(res, 400, "invalid_request", "expected ?key=<key>&state=locked|unlocked");
    const auto set = achievements::FromJson(s.games.Metadata().ReadAchievements(game->id));
    const auto item = std::ranges::find(set.items, key, &achievements::Achievement::key);
    if (item == set.items.end())
      return SendError(res, 404, "achievement_not_found", "no such achievement");
    // A store that has only one picture uses it for both.
    const std::string& url =
        state == "locked" ? (item->icon_locked.empty() ? item->icon_unlocked : item->icon_locked)
                          : (item->icon_unlocked.empty() ? item->icon_locked : item->icon_unlocked);
    if (url.empty()) return SendError(res, 404, "icon_not_found", "this achievement has no icon");
    const std::filesystem::path dir = s.games.Metadata().ArtworkDir(game->id) / "achievements";
    std::optional<std::filesystem::path> file = achievements::CachedIcon(dir, url);
    if (!file) {
      const auto done = std::make_shared<std::promise<Result<std::filesystem::path>>>();
      std::future<Result<std::filesystem::path>> downloaded = done->get_future();
      s.achievement_icons.Post([done, dir, url] {
        // Asked for twice at once: the second finds the first's file.
        if (auto cached = achievements::CachedIcon(dir, url)) return done->set_value(*cached);
        done->set_value(achievements::DownloadIcon(dir, url));
      });
      try {
        if (downloaded.wait_for(std::chrono::seconds(60)) != std::future_status::ready)
          return SendError(res, 504, "icon_download_failed", "the icon took too long to download");
        auto result = downloaded.get();
        if (!result) return SendError(res, 502, result.error());
        file = *result;
      } catch (const std::future_error&) {
        return SendError(res, 503, "shutting_down", "mirad is stopping");
      }
    }
    std::ifstream in(*file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return SendError(res, 404, "icon_not_found", "the icon file is missing");
    const std::string mime = metadata::ImageContentType(bytes);
    res.set_content(std::move(bytes), mime);
  });
}

}  // namespace mira::api
