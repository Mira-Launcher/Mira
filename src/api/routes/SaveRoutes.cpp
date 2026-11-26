#include <httplib.h>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "core/Json.h"
#include "saves/Backups.h"
#include "saves/Manifest.h"
#include "saves/Resolve.h"
#include "saves/Saves.h"
#include "saves/remote/Connect.h"
#include "saves/remote/RemoteSync.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

constexpr int kSearchLimit = 50;

}  // namespace

// {values} as strings; nullopt when any isn't one.
std::optional<std::map<std::string, std::string>> Strings(const json& values) {
  std::map<std::string, std::string> out;
  if (!values.is_object()) return std::nullopt;
  for (const auto& [key, value] : values.items()) {
    if (!value.is_string()) return std::nullopt;
    out[key] = value.get<std::string>();
  }
  return out;
}

void RegisterRemoteRoutes(Router& http, Services& s) {
  http.Get("/v1/saves/remote",
           [&s](const Request&, Response& res) { SendJson(res, s.RemoteState()); });

  // The body holds secrets: never logged or answered.
  http.Put(R"(/v1/saves/remote/(primary|mirror))", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape =
        R"({"provider": "...", "name": "...", "folder"?: "...", "ca_file"?: "...", "host_keys"?: "...", "values"?: {"<setting>": "..."}})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const auto values = Strings(body->value("values", json::object()));
    if (!values) return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    const std::string slot = req.matches[1];
    saves::remote::RemoteRequest request{.slot = slot,
                                         .provider = core::JsonString(*body, "provider"),
                                         .name = core::JsonString(*body, "name"),
                                         .folder = core::JsonString(*body, "folder"),
                                         .ca_file = core::JsonString(*body, "ca_file"),
                                         .values = *values};
    if (slot == "mirror" && !saves::remote::FindRemote(s.config, "primary"))
      return SendError(res, 409, "remote_none", "Connect a remote before a mirror.");
    const std::string host_keys = core::JsonString(*body, "host_keys");
    s.StartJob(req, res, "saves", "remote-" + slot, "Connecting to " + request.name,
               [&s, slot, request = std::move(request),
                host_keys](JobRegistry::Progress& progress) -> Result<json> {
                 auto connected = saves::remote::Connect(
                     s.config, request, host_keys, [&](const std::string& url) {
                       s.events.Publish("saves.remote_sign_in", {{"slot", slot}, {"url", url}});
                       progress.Report(0, 1, "Sign in at " + url);
                     });
                 if (!connected) return std::unexpected(connected.error());
                 s.events.Publish("saves.remote_changed", json::object());
                 return s.RemoteState();
               });
  });

  http.Delete(R"(/v1/saves/remote/(primary|mirror))", [&s](const Request& req, Response& res) {
    const std::string slot = req.matches[1];
    // The mirror only copies what the primary has.
    std::vector<std::string> slots = {slot};
    if (slot == "primary") slots.insert(slots.begin(), "mirror");
    for (const std::string& each : slots) {
      if (auto gone = saves::remote::Disconnect(s.config, each); !gone)
        return SendError(res, 500, gone.error());
      if (auto gone = s.games.ForgetRemoteSyncs(each); !gone)
        return SendError(res, 500, gone.error());
    }
    s.events.Publish("saves.remote_changed", json::object());
    SendJson(res, s.RemoteState());
  });

  http.Post(R"(/v1/saves/remote/(primary|mirror)/test)", [&s](const Request& req, Response& res) {
    const auto info = saves::remote::FindRemote(s.config, req.matches[1].str());
    if (!info) return SendError(res, 409, "remote_none", "No remote is connected.");
    s.StartJob(
        req, res, "saves", "remote-" + info->slot, "Checking " + info->name,
        [&s, info = *info](JobRegistry::Progress&) -> Result<json> {
          if (auto ok = saves::remote::Probe(s.config, saves::remote::RemoteFor(s.config, info));
              !ok)
            return std::unexpected(ok.error());
          return json::object();
        });
  });

  http.Post("/v1/saves/remote/sync", [&s](const Request& req, Response& res) {
    if (!saves::remote::FindRemote(s.config, "primary"))
      return SendError(res, 409, "remote_none", "No remote is connected.");
    s.StartJob(
        req, res, "saves", "remote", "Syncing saves",
        [&s](JobRegistry::Progress& progress) { return s.SyncAllRemote(progress); }, &s.saves);
  });

  http.Post("/v1/saves/remote/host-keys", [&s](const Request& req, Response& res) {
    const auto body = BodyObject(req, res, R"({"host": "...", "port"?: "22"})");
    if (!body) return;
    const std::string host = core::JsonString(*body, "host");
    const std::string port = core::JsonString(*body, "port");
    s.StartJob(
        req, res, "saves", "remote-host", "Checking " + host,
        [host, port](JobRegistry::Progress&) { return saves::remote::ScanHost(host, port); });
  });

  http.Post("/v1/saves/remote/ssh-key", [&s](const Request&, Response& res) {
    auto key = saves::remote::SshKey(s.config);
    if (!key) return SendError(res, 409, key.error());
    SendJson(res, *key);
  });
}

void RegisterSaveRoutes(Router& http, Services& s) {
  RegisterRemoteRoutes(http, s);
  // Answers from the index and the disk, never the network; fetches the list when it's due.
  http.Get(R"(/v1/games/([^/]+)/saves)", [&s](const Request& req, Response& res) {
    const auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    s.CheckSaveManifest();
    json answer = saves::Overview(s.config, s.games.Metadata(), s.games, *game);
    if (s.RemoteEligible(*game))
      answer["cloud"] = saves::remote::RemoteOverview(
          s.config, s.games, *game, saves::SurveyFor(s.config, s.games.Metadata(), *game));
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
    if (saves::CloudFor(*game) == nullptr && !s.RemoteEligible(*game))
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
