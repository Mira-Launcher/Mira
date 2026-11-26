#include "Games.h"

#include <cctype>
#include <chrono>
#include <json.hpp>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "../Async.h"
#include "../Jobs.h"
#include "../JsonMapping.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
namespace {

using nlohmann::json;

LaunchResult LaunchGameSync(const std::string& id, const std::string& option,
                            const std::string& saves) {
  const std::string path = "/v1/games/" + PercentEncode(id) + "/launch";
  json body = json::object();
  if (!option.empty()) body["option"] = option;
  if (!saves.empty()) body["saves"] = saves;
  const transport::Reply reply =
      body.empty() ? transport::Post(path) : transport::PostJson(path, body);
  // `tracked` (docs/api.md): whether game.state events are coming for this launch.
  bool tracked = true;
  std::string warning;
  if (reply.body.is_object()) {
    const json& body = reply.body;
    warning = body.value("warning", std::string());
    if (body.contains("tracked") && body["tracked"].is_boolean()) {
      tracked = body["tracked"].get<bool>();
    }
  }
  return {reply.ok, reply.error, tracked, warning};
}

StopResult StopGameSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + PercentEncode(id) + "/stop");
  return {reply.ok, reply.error};
}

void FillGameDetail(GameDetailResult& result, const json& body) {
  result.game = mapping::ToGameDetail(body);
}

GameDetailResult GetGameSync(const std::string& id) {
  return ReadReply<GameDetailResult>(transport::Get("/v1/games/" + PercentEncode(id)),
                                     "GET /v1/games/" + id, Shape::Object, FillGameDetail);
}

PatchGameResult PatchGameSync(const std::string& id, const GamePatch& patch) {
  PatchGameResult result;
  json body = json::object();
  if (patch.name) body["name"] = *patch.name;
  if (patch.exe_path) body["exe_path"] = *patch.exe_path;
  if (patch.args) body["args"] = *patch.args;
  if (patch.working_dir) body["working_dir"] = *patch.working_dir;
  if (patch.runner_ref) body["runner_ref"] = *patch.runner_ref;
  if (patch.data_dir) body["data_dir"] = *patch.data_dir;
  if (patch.install_path) body["install_path"] = *patch.install_path;
  if (patch.tags) body["tags"] = *patch.tags;
  if (patch.launch) body["launch"] = mapping::ToJson(*patch.launch);
  if (patch.drop_prefix) body["drop_prefix"] = true;

  const auto parse_object = [&](const std::string& text, const char* field,
                                const char* message) -> bool {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      result.error = message;
      return false;
    }
    body[field] = parsed;
    return true;
  };
  if (patch.runner_config_json && !parse_object(*patch.runner_config_json, "runner_config",
                                                "Runner config must be a JSON object, e.g. {}")) {
    return result;
  }
  if (patch.env_json && !parse_object(*patch.env_json, "env",
                                      "Environment must be a JSON object of strings, e.g. {}")) {
    return result;
  }

  const transport::Reply reply = transport::Patch("/v1/games/" + PercentEncode(id), body);
  return {reply.ok, reply.error};
}

GameConfigResult GetGameConfigSync(const std::string& id) {
  return ReadReply<GameConfigResult>(transport::Get("/v1/games/" + PercentEncode(id) + "/config"),
                                     "GET /v1/games/" + id + "/config", Shape::Object,
                                     [](GameConfigResult& result, const json& body) {
                                       // Schema::Entries() order.
                                       for (const auto& [key, entry] : body.items()) {
                                         GameConfigEntry e;
                                         e.key = key;
                                         e.value_display =
                                             mapping::ToDisplayString(entry.value("value", json()));
                                         e.layer = entry.value("layer", std::string());
                                         e.overridable = entry.value("overridable", false);
                                         e.applies = entry.value("applies", true);
                                         result.entries.push_back(std::move(e));
                                       }
                                     });
}

GameActionResult DeleteInstallerSync(const std::string& id) {
  const transport::Reply reply = transport::Delete("/v1/games/" + PercentEncode(id) + "/installer");
  return {reply.ok, reply.error};
}

RunInPrefixResult RunInPrefixSync(const std::string& id, const std::string& exe_path,
                                  const std::string& args) {
  const transport::Reply reply = transport::PostJson(
      "/v1/games/" + PercentEncode(id) + "/run", json{{"exe_path", exe_path}, {"args", args}},
      // Provisions a prefix on demand if there isn't one yet, which is
      // genuinely slow (it's initialising Wine/Proton, see
      // docs/backend.md).
      {.read_timeout = std::chrono::seconds(120)});
  return {reply.ok, reply.error};
}

PatchGameResult DetectGameSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + PercentEncode(id) + "/detect");
  return {reply.ok, reply.error};
}

PatchGameConfigResult PatchGameConfigSync(const std::string& id,
                                          const std::vector<GameConfigEdit>& edits) {
  json body = json::object();
  for (const GameConfigEdit& edit : edits) {
    body[edit.key] =
        edit.clear ? json(nullptr) : mapping::TypedValueFromText(edit.type, edit.value);
  }
  const transport::Reply reply =
      transport::Patch("/v1/games/" + PercentEncode(id) + "/config", body);
  return {reply.ok, reply.error};
}

PatchGamesResult PatchGamesSync(const GamesPatch& patch) {
  PatchGamesResult result;
  json config = json::object();
  for (const GameConfigEdit& edit : patch.config) {
    config[edit.key] =
        edit.clear ? json(nullptr) : mapping::TypedValueFromText(edit.type, edit.value);
  }
  json body = {{"ids", patch.ids},
               {"add_tags", patch.add_tags},
               {"remove_tags", patch.remove_tags},
               {"config", config}};
  if (patch.folder_tag) body["folder_tag"] = *patch.folder_tag;
  const transport::Reply reply = transport::Patch("/v1/games", body);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  if (reply.body.is_object() && reply.body.contains("games") && reply.body["games"].is_array()) {
    for (const json& entry : reply.body["games"])
      result.games.push_back(mapping::ToGameSummary(entry));
  }
  return result;
}

GameSessionsResult GetGameSessionsSync(const std::string& id, int limit) {
  return ReadReply<GameSessionsResult>(
      transport::Get("/v1/games/" + PercentEncode(id) + "/sessions?limit=" + std::to_string(limit)),
      "GET /v1/games/" + id + "/sessions", Shape::Object,
      [](GameSessionsResult& result, const json& body) {
        if (!body.contains("sessions") || !body["sessions"].is_array()) return;
        for (const json& item : body["sessions"]) {
          GameSession session;
          session.started_at = item.value("started_at", std::int64_t{0});
          session.ended_at = item.value("ended_at", std::int64_t{0});
          session.duration_seconds = item.value("duration_seconds", std::int64_t{0});
          result.sessions.push_back(session);
        }
      });
}

AchievementsResult GetAchievementsSync(const std::string& id) {
  const transport::Reply reply = transport::Get("/v1/games/" + PercentEncode(id) + "/achievements");
  if (reply.status == 404 && reply.error.code == "no_achievements") {
    AchievementsResult result;
    result.missing = true;
    return result;
  }
  return ReadReply<AchievementsResult>(
      reply, "GET /v1/games/" + id + "/achievements", Shape::Object,
      [](AchievementsResult& result, const json& body) {
        result.source = body.value("source", std::string());
        result.fetched_at = body.value("fetched_at", std::int64_t{0});
        result.total = body.value("total", 0);
        result.unlocked = body.value("unlocked", 0);
        for (const json& item : body.value("items", json::array())) {
          if (!item.is_object()) continue;
          result.items.push_back({.key = item.value("key", std::string()),
                                  .name = item.value("name", std::string()),
                                  .description = item.value("description", std::string()),
                                  .hidden = item.value("hidden", false),
                                  .unlocked = item.value("unlocked", false),
                                  .unlocked_at = item.value("unlocked_at", std::int64_t{0}),
                                  .rarity = item.value("rarity", -1.0),
                                  .icon = item.value("icon", std::string())});
        }
      });
}

TricksResult RunWinetricksSync(const std::string& id, const std::string& verb) {
  const transport::Reply reply =
      transport::PostJson("/v1/games/" + PercentEncode(id) + "/tricks", json{{"verb", verb}});
  return {reply.ok, reply.error};
}

GameDetailResult AddManualGameSync(const std::string& install_path, const std::string& exe_path,
                                   const std::string& name, const std::string& platform,
                                   bool is_installer, bool app) {
  json body{{"install_path", install_path},
            {"exe_path", exe_path},
            {"is_installer", is_installer},
            {"app", app}};
  if (!name.empty()) body["name"] = name;
  if (!platform.empty()) body["platform"] = platform;

  return ReadReply<GameDetailResult>(transport::PostJson("/v1/games/manual", body),
                                     "POST /v1/games/manual", Shape::Object, FillGameDetail);
}

InstallerInfoResult GetInstallerInfoSync(const std::string& id, const std::string& path) {
  std::string url = "/v1/games/" + PercentEncode(id) + "/installer";
  if (!path.empty()) url += "?path=" + PercentEncode(path);
  return ReadReply<InstallerInfoResult>(transport::Get(url), "GET /v1/games/" + id + "/installer",
                                        Shape::Object,
                                        [](InstallerInfoResult& result, const json& body) {
                                          InstallerInfoResult info = mapping::ToInstallerInfo(body);
                                          info.ok = result.ok;
                                          info.error = std::move(result.error);
                                          result = std::move(info);
                                        });
}

GameActionResult InstallGameSync(const std::string& id, bool interactive,
                                 const std::string& installer, const std::string& target) {
  json body = {{"interactive", interactive}};
  if (!installer.empty()) body["installer"] = installer;
  if (!target.empty()) body["target"] = target;
  const transport::Reply reply =
      transport::PostJson("/v1/games/" + PercentEncode(id) + "/install", body);
  return {reply.ok, reply.error};
}

InstallProgressResult GetInstallProgressSync(const std::string& id) {
  return ReadReply<InstallProgressResult>(
      transport::Get("/v1/games/" + PercentEncode(id) + "/install/progress"),
      "GET /v1/games/" + id + "/install/progress", Shape::Object,
      [](InstallProgressResult& result, const json& body) {
        result.state = body.value("state", std::string("idle"));
        result.bytes_written = body.value("bytes_written", std::int64_t{0});
        result.estimated_bytes = body.value("estimated_bytes", std::int64_t{-1});
        result.progress = body.value("progress", -1.0);
      });
}

void FillDeleteGames(DeleteGamesResult& result, const json& body) {
  if (body.is_object() && body.contains("removed") && body["removed"].is_array()) {
    for (const json& id : body["removed"]) {
      if (id.is_string()) result.removed.push_back(id.get<std::string>());
    }
  }
  result.failed = mapping::ToGameFailures(body, "failed");
}

}  // namespace

void LaunchGameAsync(QObject* context, const std::string& id, const std::string& option,
                     const std::string& saves, std::function<void(LaunchResult)> callback) {
  async::Run(
      context, [id, option, saves] { return LaunchGameSync(id, option, saves); },
      std::move(callback));
}

void StopGameAsync(QObject* context, const std::string& id,
                   std::function<void(StopResult)> callback) {
  async::Run(context, [id] { return StopGameSync(id); }, std::move(callback));
}

void GetGameAsync(QObject* context, const std::string& id,
                  std::function<void(GameDetailResult)> callback) {
  async::Run(context, [id] { return GetGameSync(id); }, std::move(callback));
}

void PatchGameAsync(QObject* context, const std::string& id, const GamePatch& patch,
                    std::function<void(PatchGameResult)> callback) {
  async::Run(context, [id, patch] { return PatchGameSync(id, patch); }, std::move(callback));
}

void DetectGameAsync(QObject* context, const std::string& id,
                     std::function<void(PatchGameResult)> callback) {
  async::Run(context, [id] { return DetectGameSync(id); }, std::move(callback), async::Lane::Slow);
}

void RunInPrefixAsync(QObject* context, const std::string& id, const std::string& exe_path,
                      const std::string& args, std::function<void(RunInPrefixResult)> callback) {
  async::Run(
      context, [id, exe_path, args] { return RunInPrefixSync(id, exe_path, args); },
      std::move(callback), async::Lane::Slow);
}

void ConfirmGameAsync(QObject* context, const std::string& id, const SetupChoices& choices,
                      std::function<void(GameDetailResult)> callback) {
  async::Run(
      context,
      [id, choices] {
        return ReadReply<GameDetailResult>(
            transport::PostJson("/v1/games/" + PercentEncode(id) + "/confirm",
                                mapping::FromSetupChoices(choices)),
            "POST /v1/games/{id}/confirm", Shape::Object,
            [](GameDetailResult& result, const json& body) {
              result.game = mapping::ToGameDetail(body);
            });
      },
      std::move(callback), async::Lane::Slow);
}

void GetGameConfigAsync(QObject* context, const std::string& id,
                        std::function<void(GameConfigResult)> callback) {
  async::Run(context, [id] { return GetGameConfigSync(id); }, std::move(callback));
}

void PatchGameConfigAsync(QObject* context, const std::string& id,
                          const std::vector<GameConfigEdit>& edits,
                          std::function<void(PatchGameConfigResult)> callback) {
  async::Run(context, [id, edits] { return PatchGameConfigSync(id, edits); }, std::move(callback));
}

void PatchGamesAsync(QObject* context, const GamesPatch& patch,
                     std::function<void(PatchGamesResult)> callback) {
  async::Run(context, [patch] { return PatchGamesSync(patch); }, std::move(callback));
}

void GetGameSessionsAsync(QObject* context, const std::string& id, int limit,
                          std::function<void(GameSessionsResult)> callback) {
  async::Run(context, [id, limit] { return GetGameSessionsSync(id, limit); }, std::move(callback));
}

void GetAchievementsAsync(QObject* context, const std::string& id,
                          std::function<void(AchievementsResult)> callback) {
  async::Run(context, [id] { return GetAchievementsSync(id); }, std::move(callback));
}

void RefreshAchievementsAsync(QObject* context, const std::string& id,
                              std::function<void(AchievementsRefreshResult)> callback) {
  RunJob<AchievementsRefreshResult>(
      context, "achievements",
      [id](const std::string& query) {
        return transport::Post("/v1/games/" + PercentEncode(id) + "/achievements/refresh" + query);
      },
      [](AchievementsRefreshResult& result, const json& body) {
        result.total = body.value("total", 0);
        result.unlocked = body.value("unlocked", 0);
      },
      std::move(callback));
}

void GetSavesAsync(QObject* context, const std::string& id,
                   std::function<void(SavesResult)> callback) {
  async::Run(
      context,
      [id] {
        return ReadReply<SavesResult>(
            transport::Get("/v1/games/" + PercentEncode(id) + "/saves"),
            "GET /v1/games/" + id + "/saves", Shape::Object,
            [](SavesResult& result, const json& body) { result = mapping::ToSaves(body); });
      },
      std::move(callback));
}

void BackUpSavesAsync(QObject* context, const std::string& id,
                      std::function<void(SavesJobResult)> callback) {
  RunJob<SavesJobResult>(
      context, "saves",
      [id](const std::string& query) {
        return transport::Post("/v1/games/" + PercentEncode(id) + "/saves/backup" + query);
      },
      [](SavesJobResult& result, const json& body) {
        result.snapshot = mapping::Str(body, "snapshot");
        result.taken = body.value("taken", false);
      },
      std::move(callback));
}

void RestoreSavesAsync(QObject* context, const std::string& id, const std::string& snapshot,
                       std::function<void(SavesJobResult)> callback) {
  RunJob<SavesJobResult>(
      context, "saves",
      [id, snapshot](const std::string& query) {
        return transport::PostJson("/v1/games/" + PercentEncode(id) + "/saves/restore" + query,
                                   json{{"snapshot", snapshot}});
      },
      [](SavesJobResult& result, const json& body) {
        result.snapshot = mapping::Str(body, "restored");
        result.undo = mapping::Str(body, "undo");  // null when there were no saves
      },
      std::move(callback));
}

void DeleteSaveBackupAsync(QObject* context, const std::string& id, const std::string& snapshot,
                           std::function<void(GameActionResult)> callback) {
  async::Run(
      context,
      [id, snapshot] {
        const transport::Reply reply = transport::Delete("/v1/games/" + PercentEncode(id) +
                                                         "/saves/" + PercentEncode(snapshot));
        return GameActionResult{reply.ok, reply.error};
      },
      std::move(callback));
}

void PinSaveBackupAsync(QObject* context, const std::string& id, const std::string& snapshot,
                        bool pinned, std::function<void(GameActionResult)> callback) {
  async::Run(
      context,
      [id, snapshot, pinned] {
        const transport::Reply reply =
            transport::Patch("/v1/games/" + PercentEncode(id) + "/saves/" + PercentEncode(snapshot),
                             json{{"pinned", pinned}});
        return GameActionResult{reply.ok, reply.error};
      },
      std::move(callback));
}

void ChangeSaveFolderAsync(QObject* context, const std::string& id, bool add,
                           const std::string& folder,
                           std::function<void(GameActionResult)> callback,
                           const std::string& matching) {
  async::Run(
      context,
      [id, add, folder, matching] {
        json body = {{add ? "add" : "remove", folder}};
        if (!matching.empty()) body["matching"] = matching;
        const transport::Reply reply =
            transport::PostJson("/v1/games/" + PercentEncode(id) + "/saves/folders", body);
        return GameActionResult{reply.ok, reply.error};
      },
      std::move(callback));
}

void SyncCloudSavesAsync(QObject* context, const std::string& id, const std::string& action,
                         std::function<void(SavesJobResult)> callback) {
  RunJob<SavesJobResult>(
      context, "saves",
      [id, action](const std::string& query) {
        return transport::PostJson("/v1/games/" + PercentEncode(id) + "/saves/cloud" + query,
                                   json{{"action", action}});
      },
      [](SavesJobResult& result, const json& body) { result.state = mapping::Str(body, "state"); },
      std::move(callback));
}

void RunWinetricksAsync(QObject* context, const std::string& id, const std::string& verb,
                        std::function<void(TricksResult)> callback) {
  async::Run(context, [id, verb] { return RunWinetricksSync(id, verb); }, std::move(callback));
}

void GetInstalledTricksAsync(QObject* context, const std::string& id,
                             std::function<void(InstalledTricksResult)> callback) {
  async::Run(
      context,
      [id] {
        return ReadReply<InstalledTricksResult>(
            transport::Get("/v1/games/" + PercentEncode(id) + "/tricks"),
            "GET /v1/games/" + id + "/tricks", Shape::Object,
            [](InstalledTricksResult& result, const json& body) {
              for (const json& verb : body.value("installed", json::array())) {
                if (verb.is_string()) result.installed.push_back(verb.get<std::string>());
              }
            });
      },
      std::move(callback));
}

void AddManualGameAsync(QObject* context, const std::string& install_path,
                        const std::string& exe_path, const std::string& name,
                        const std::string& platform, bool is_installer, bool app,
                        std::function<void(GameDetailResult)> callback) {
  async::Run(
      context,
      [install_path, exe_path, name, platform, is_installer, app] {
        return AddManualGameSync(install_path, exe_path, name, platform, is_installer, app);
      },
      std::move(callback));
}

void GetInstallerInfoAsync(QObject* context, const std::string& id, const std::string& path,
                           std::function<void(InstallerInfoResult)> callback) {
  async::Run(context, [id, path] { return GetInstallerInfoSync(id, path); }, std::move(callback));
}

void InstallGameAsync(QObject* context, const std::string& id, bool interactive,
                      const std::string& installer, const std::string& target,
                      std::function<void(GameActionResult)> callback) {
  async::Run(
      context,
      [id, interactive, installer, target] {
        return InstallGameSync(id, interactive, installer, target);
      },
      std::move(callback));
}

void GetInstallProgressAsync(QObject* context, const std::string& id,
                             std::function<void(InstallProgressResult)> callback) {
  async::Run(context, [id] { return GetInstallProgressSync(id); }, std::move(callback));
}

void DeleteGamesAsync(QObject* context, const std::vector<std::string>& ids, bool delete_files,
                      bool delete_prefix, bool delete_saves,
                      std::function<void(DeleteGamesResult)> callback) {
  const json body = {{"ids", ids},
                     {"delete_files", delete_files},
                     {"delete_prefix", delete_prefix},
                     {"delete_saves", delete_saves}};
  RunJob<DeleteGamesResult>(
      context, "delete",
      [body](const std::string& query) {
        return transport::PostJson("/v1/games/delete" + query, body);
      },
      FillDeleteGames, std::move(callback));
}

void GetRemovedGamesAsync(QObject* context, bool all,
                          std::function<void(RemovedGamesResult)> callback) {
  async::Run(
      context,
      [all] {
        const std::string path = all ? "/v1/removed-games?all=true" : "/v1/removed-games";
        return ReadReply<RemovedGamesResult>(
            transport::Get(path), "GET /v1/removed-games", Shape::Array,
            [](RemovedGamesResult& result, const json& body) {
              for (const json& entry : body) {
                RemovedGame game{.id = entry.at("id").get<std::string>(),
                                 .name = entry.value("name", std::string()),
                                 .source = entry.value("source", std::string()),
                                 .tags = {},
                                 .removed_at = entry.value("removed_at", std::int64_t{0}),
                                 .play_seconds = entry.value("play_seconds", std::int64_t{0}),
                                 .last_played_at = std::nullopt,
                                 .install_path = entry.value("install_path", std::string()),
                                 .exe_path = entry.value("exe_path", std::string()),
                                 .prefix_bytes = entry.value("prefix_bytes", std::uint64_t{0}),
                                 .saves_bytes = entry.value("saves_bytes", std::uint64_t{0})};
                if (const auto played = entry.value("last_played_at", std::int64_t{0}); played > 0)
                  game.last_played_at = played;
                if (const auto tags = entry.find("tags"); tags != entry.end() && tags->is_array())
                  for (const json& tag : *tags)
                    if (tag.is_string()) game.tags.push_back(tag.get<std::string>());
                result.games.push_back(std::move(game));
              }
            });
      },
      std::move(callback));
}

void DeleteRemovedGameAsync(QObject* context, const std::string& id,
                            std::function<void(StoreActionResult)> callback) {
  async::Run(
      context,
      [id] {
        const auto reply = transport::Delete("/v1/removed-games/" + PercentEncode(id));
        return StoreActionResult{reply.ok, reply.error};
      },
      std::move(callback));
}

void GetRemovalPlansAsync(QObject* context, const std::vector<std::string>& ids,
                          std::function<void(RemovalPlansResult)> callback) {
  async::Run(
      context,
      [ids] {
        return ReadReply<RemovalPlansResult>(
            transport::PostJson("/v1/games/removal", {{"ids", ids}}), "POST /v1/games/removal",
            Shape::Array, [](RemovalPlansResult& result, const json& body) {
              for (const json& entry : body) {
                result.plans.push_back(
                    {.id = entry.at("id").get<std::string>(),
                     .name = entry.value("name", std::string()),
                     .kind = entry.value("kind", std::string()),
                     .tool = entry.value("tool", std::string()),
                     .files = entry.value("files", std::string()),
                     .files_in_prefix = entry.value("files_in_prefix", std::string()),
                     .folder_is_prefix = entry.value("folder_is_prefix", false),
                     .prefix = entry.value("prefix", std::string()),
                     .prefix_shared_with = entry.value("prefix_shared_with", std::string()),
                     .saves = mapping::Str(entry, "saves")});  // null without backups
              }
            });
      },
      std::move(callback));
}

void GetRelocationPlansAsync(QObject* context, const std::vector<std::string>& ids,
                             std::function<void(RelocationPlansResult)> callback) {
  async::Run(
      context,
      [ids] {
        return ReadReply<RelocationPlansResult>(
            transport::PostJson("/v1/games/relocation", {{"ids", ids}}),
            "POST /v1/games/relocation", Shape::Array,
            [](RelocationPlansResult& result, const json& body) {
              const auto part = [](const json& entry) {
                return RelocationPlan::Part{.action = entry.value("action", std::string("none")),
                                            .from = entry.value("from", std::string()),
                                            .to = entry.value("to", std::string()),
                                            .reason = entry.value("reason", std::string()),
                                            .copies = entry.value("copies", false)};
              };
              for (const json& entry : body) {
                RelocationPlan plan;
                plan.id = entry.value("id", std::string());
                plan.name = entry.value("name", std::string());
                if (entry.contains("error")) {
                  plan.ok = false;
                  plan.error = mapping::ToApiError(entry["error"]);
                } else {
                  plan.files = part(entry.value("files", json::object()));
                  plan.prefix = part(entry.value("prefix", json::object()));
                }
                result.plans.push_back(std::move(plan));
              }
            });
      },
      std::move(callback));
}

void GetRemovalSizesAsync(QObject* context, const std::vector<std::string>& ids,
                          std::function<void(RemovalSizesResult)> callback) {
  const json body = {{"ids", ids}};
  RunJob<RemovalSizesResult>(
      context, "removal-sizes",
      [body](const std::string& query) {
        return transport::PostJson("/v1/games/removal/sizes" + query, body);
      },
      [](RemovalSizesResult& result, const json& sizes) {
        for (const auto& [id, size] : sizes.items()) {
          result.sizes[id] = {.files = size.value("files", std::uint64_t{0}),
                              .prefix = size.value("prefix", std::uint64_t{0}),
                              .saves = size.value("saves", std::uint64_t{0})};
        }
      },
      std::move(callback));
}

void DeleteInstallerAsync(QObject* context, const std::string& id,
                          std::function<void(GameActionResult)> callback) {
  async::Run(context, [id] { return DeleteInstallerSync(id); }, std::move(callback));
}

}  // namespace mira_gui::api
