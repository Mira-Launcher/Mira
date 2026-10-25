#include <httplib.h>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "core/Json.h"
#include "saves/Backups.h"
#include "saves/Manifest.h"
#include "saves/Resolve.h"
#include "saves/Saves.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

constexpr int kSearchLimit = 50;

}  // namespace

void RegisterSaveRoutes(Router& http, Services& s) {
  // Answers from the index and the disk, never the network; fetches the list when it's due.
  http.Get(R"(/v1/games/([^/]+)/saves)", [&s](const Request& req, Response& res) {
    const auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    s.CheckSaveManifest();
    json answer = saves::Overview(s.config, s.games.Metadata(), s.games, *game);
    answer["doing"] = s.SavesDoing(game->id);
    SendJson(res, answer);
  });

  http.Post(R"(/v1/games/([^/]+)/saves/backup)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    const auto game = s.games.Find(id);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    s.StartJob(
        req, res, "saves", id, std::format("Backing up {}'s saves", game->name),
        [&s, id](JobRegistry::Progress&) { return s.BackUpSaves(id, saves::kManual); }, &s.saves);
  });

  http.Post(R"(/v1/games/([^/]+)/saves/restore)", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"snapshot": "<id>"})";
    const std::string id = req.matches[1];
    const auto game = s.games.Find(id);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const std::string snapshot = body->value("snapshot", std::string());
    if (!saves::FindSnapshot(saves::GameBackups(s.config, id), snapshot))
      return SendError(res, 404, "snapshot_not_found", "no such backup");
    s.StartJob(
        req, res, "saves", id, std::format("Restoring {}'s saves", game->name),
        [&s, id, snapshot](JobRegistry::Progress&) { return s.RestoreSaves(id, snapshot); },
        &s.saves);
  });

  http.Post(R"(/v1/games/([^/]+)/saves/cloud)", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"action": "check" | "sync" | "download" | "upload"})";
    const std::string id = req.matches[1];
    const auto game = s.games.Find(id);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    static const std::map<std::string, saves::CloudAction> kActions = {
        {"check", saves::CloudAction::Check},
        {"sync", saves::CloudAction::Sync},
        {"download", saves::CloudAction::Download},
        {"upload", saves::CloudAction::Upload}};
    const auto action = kActions.find(body->value("action", std::string()));
    if (action == kActions.end())
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    if (saves::CloudFor(*game) == nullptr)
      return SendError(res, 409, "cloud_unsupported",
                       std::format("{} has no cloud saves Mira syncs.", game->name));
    s.StartJob(
        req, res, "saves", id, std::format("Syncing {}'s saves", game->name),
        [&s, id, action = action->second](JobRegistry::Progress&) {
          return s.SyncCloudSaves(id, action);
        },
        &s.saves);
  });

  // One folder at a time, changed in the stored list itself, so two quick edits both land.
  http.Post(R"(/v1/games/([^/]+)/saves/folders)", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"add": "<path>"} or {"remove": "<pattern>"})";
    const std::string id = req.matches[1];
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const std::string add = core::JsonString(*body, "add");
    const std::string remove = core::JsonString(*body, "remove");
    if (add.empty() == remove.empty())
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    auto updated = s.games.Update(id, [&](model::Game& game) {
      json kept = json::array();
      if (const auto found = game.overrides.find("saves.extra_paths");
          found != game.overrides.end() && found->is_array())
        kept = *found;
      // Written against the game's folders, so they follow it when it or its prefix moves.
      const std::string pattern =
          add.empty() ? remove
                      : saves::Shorten(saves::Escape(add), saves::RootsFor(s.config, game));
      kept.erase(std::remove(kept.begin(), kept.end(), json(pattern)), kept.end());
      if (!add.empty()) kept.push_back(pattern);
      // None left clears the game's own value rather than keeping an empty one.
      if (kept.empty())
        game.overrides.erase("saves.extra_paths");
      else
        game.overrides["saves.extra_paths"] = std::move(kept);
    });
    if (!updated) return SendStoreError(res, updated.error());
    s.events.Publish("game.updated", s.Record(*updated));
    SendJson(res, {{"extra_paths", updated->overrides.value("saves.extra_paths", json::array())}});
  });

  // Not a claim, as for a delete: pinning changes no saves.
  http.Patch(R"(/v1/games/([^/]+)/saves/([^/]+))", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"pinned": true | false})";
    const std::string id = req.matches[1];
    if (!s.games.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const auto pinned = body->find("pinned");
    if (pinned == body->end() || !pinned->is_boolean())
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    if (auto done = saves::PinSnapshot(s.config, id, req.matches[2].str(), pinned->get<bool>());
        !done)
      return SendError(res, done.error().code == "snapshot_not_found" ? 404 : 500, done.error());
    s.events.Publish("game.saves_changed", {{"id", id}});
    SendJson(res, json::object());
  });

  // Not a claim: a backup can go while the game runs.
  http.Delete(R"(/v1/games/([^/]+)/saves/([^/]+))", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!s.games.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    if (auto deleted = saves::DeleteSnapshot(s.config, id, req.matches[2].str()); !deleted)
      return SendError(res, deleted.error().code == "snapshot_not_found" ? 404 : 500,
                       deleted.error());
    s.events.Publish("game.saves_changed", {{"id", id}});
    SendJson(res, json::object());
  });

  http.Get("/v1/saves/manifest", [&s](const Request&, Response& res) {
    SendJson(res, saves::ManifestState(s.config, s.games.Metadata()));
  });

  http.Post("/v1/saves/manifest/update", [&s](const Request& req, Response& res) {
    SendJson(res, {{"status", "running"}, {"job", s.UpdateSaveManifest(Param(req, "job"))}}, 202);
  });

  // For picking a game's entry by hand. Fetches the list when it's due, answering from the one
  // there is meanwhile.
  http.Get("/v1/saves/entries", [&s](const Request& req, Response& res) {
    s.CheckSaveManifest();
    json entries = json::array();
    for (const auto& entry :
         s.games.Metadata().SearchSaveEntries(saves::Normalize(Param(req, "q")), kSearchLimit)) {
      json item = {{"name", entry.name}};
      if (!entry.alias_of.empty()) item["alias_of"] = entry.alias_of;
      entries.push_back(std::move(item));
    }
    SendJson(res, {{"entries", std::move(entries)}});
  });
}

}  // namespace mira::api
