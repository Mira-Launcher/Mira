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

std::string DeletePath(const std::string& id, bool delete_files, bool delete_prefix,
                       bool delete_metadata) {
  // Every flag is opt-in server-side too: the bare DELETE never touches
  // disk, so an omitted param and "false" mean the same thing.
  std::string path = "/v1/games/" + PercentEncode(id);
  std::string separator = "?";
  if (delete_files) {
    path += separator + "delete_files=true";
    separator = "&";
  }
  if (delete_prefix) {
    path += separator + "delete_prefix=true";
    separator = "&";
  }
  if (delete_metadata) path += separator + "delete_metadata=true";
  return path;
}

DeleteResult DeleteGameSync(const std::string& id, bool delete_files, bool delete_prefix,
                            bool delete_metadata) {
  const transport::Reply reply =
      transport::Delete(DeletePath(id, delete_files, delete_prefix, delete_metadata));
  return {reply.ok, reply.error};
}

LaunchResult LaunchGameSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + PercentEncode(id) + "/launch");
  // `tracked` (docs/api.md): whether game.state events are coming for this
  // launch. An older mirad only says `status`; with neither, assume so.
  bool tracked = true;
  if (reply.body.is_object()) {
    const json& body = reply.body;
    if (body.contains("tracked") && body["tracked"].is_boolean()) {
      tracked = body["tracked"].get<bool>();
    } else if (body.contains("status") && body["status"].is_string()) {
      tracked = body["status"].get<std::string>() != "launched_via_steam";
    }
  }
  return {reply.ok, reply.error, tracked};
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
  if (patch.tags) body["tags"] = *patch.tags;
  if (patch.reviewed) body["reviewed"] = *patch.reviewed;

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
      // docs/architecture.md).
      {.read_timeout = std::chrono::seconds(120)});
  return {reply.ok, reply.error};
}

FinishInstallResult FinishInstallSync(const std::string& id, const std::string& install_path,
                                      const std::string& exe_path) {
  json body = json::object();
  if (!install_path.empty()) body["install_path"] = install_path;
  if (!exe_path.empty()) body["exe_path"] = exe_path;
  const transport::Reply reply =
      body.empty()
          ? transport::Post("/v1/games/" + PercentEncode(id) + "/finish-install")
          : transport::PostJson("/v1/games/" + PercentEncode(id) + "/finish-install", body);
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
  const json body = {{"ids", patch.ids},
                     {"add_tags", patch.add_tags},
                     {"remove_tags", patch.remove_tags},
                     {"config", config}};
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

GameLogResult GetGameLogSync(const std::string& id, int lines) {
  return ReadReply<GameLogResult>(
      transport::Get("/v1/games/" + PercentEncode(id) + "/log?lines=" + std::to_string(lines)),
      "GET /v1/games/" + id + "/log", Shape::Object, [](GameLogResult& result, const json& body) {
        if (!body.contains("lines") || !body["lines"].is_array()) return;
        for (const json& line : body["lines"]) {
          if (line.is_string()) result.lines.push_back(line.get<std::string>());
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
                                   bool is_installer) {
  json body{{"install_path", install_path}, {"exe_path", exe_path}, {"is_installer", is_installer}};
  if (!name.empty()) body["name"] = name;
  if (!platform.empty()) body["platform"] = platform;

  return ReadReply<GameDetailResult>(transport::PostJson("/v1/games/manual", body),
                                     "POST /v1/games/manual", Shape::Object, FillGameDetail);
}

InstallerInfoResult GetInstallerInfoSync(const std::string& id, const std::string& path) {
  std::string url = "/v1/games/" + PercentEncode(id) + "/installer";
  if (!path.empty()) url += "?path=" + PercentEncode(path);
  return ReadReply<InstallerInfoResult>(
      transport::Get(url), "GET /v1/games/" + id + "/installer", Shape::Object,
      [](InstallerInfoResult& result, const json& body) {
        result.path = body.value("path", std::string());
        result.size_bytes = body.value("size_bytes", std::int64_t{0});
        result.format = body.value("format", std::string("unknown"));
        result.silent = body.value("silent", false);
      });
}

GameActionResult InstallGameSync(const std::string& id, bool interactive,
                                 const std::string& installer) {
  json body = {{"interactive", interactive}};
  if (!installer.empty()) body["installer"] = installer;
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

void DeleteGameAsync(QObject* context, const std::string& id, bool delete_files, bool delete_prefix,
                     bool delete_metadata, std::function<void(DeleteResult)> callback) {
  if (!delete_files && !delete_prefix) {
    async::Run(
        context,
        [id, delete_metadata] { return DeleteGameSync(id, false, false, delete_metadata); },
        std::move(callback), async::Lane::Slow);
    return;
  }
  // Deleting files or a prefix is a job.
  RunJob<DeleteResult>(
      context, "delete",
      [id, delete_files, delete_prefix, delete_metadata](const std::string& query) {
        return transport::Delete(DeletePath(id, delete_files, delete_prefix, delete_metadata) +
                                 "&" + query.substr(1));
      },
      [](DeleteResult&, const json&) {}, std::move(callback));
}

void LaunchGameAsync(QObject* context, const std::string& id,
                     std::function<void(LaunchResult)> callback) {
  async::Run(context, [id] { return LaunchGameSync(id); }, std::move(callback));
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

void RunInPrefixAsync(QObject* context, const std::string& id, const std::string& exe_path,
                      const std::string& args, std::function<void(RunInPrefixResult)> callback) {
  async::Run(
      context, [id, exe_path, args] { return RunInPrefixSync(id, exe_path, args); },
      std::move(callback), async::Lane::Slow);
}

void FinishInstallAsync(QObject* context, const std::string& id,
                        std::function<void(FinishInstallResult)> callback,
                        const std::string& install_path, const std::string& exe_path) {
  async::Run(
      context,
      [id, install_path, exe_path] { return FinishInstallSync(id, install_path, exe_path); },
      std::move(callback));
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

void GetGameLogAsync(QObject* context, const std::string& id, int lines,
                     std::function<void(GameLogResult)> callback) {
  async::Run(context, [id, lines] { return GetGameLogSync(id, lines); }, std::move(callback));
}

void RunWinetricksAsync(QObject* context, const std::string& id, const std::string& verb,
                        std::function<void(TricksResult)> callback) {
  async::Run(context, [id, verb] { return RunWinetricksSync(id, verb); }, std::move(callback));
}

void AddManualGameAsync(QObject* context, const std::string& install_path,
                        const std::string& exe_path, const std::string& name,
                        const std::string& platform, bool is_installer,
                        std::function<void(GameDetailResult)> callback) {
  async::Run(
      context,
      [install_path, exe_path, name, platform, is_installer] {
        return AddManualGameSync(install_path, exe_path, name, platform, is_installer);
      },
      std::move(callback));
}

void GetInstallerInfoAsync(QObject* context, const std::string& id, const std::string& path,
                           std::function<void(InstallerInfoResult)> callback) {
  async::Run(context, [id, path] { return GetInstallerInfoSync(id, path); }, std::move(callback));
}

void InstallGameAsync(QObject* context, const std::string& id, bool interactive,
                      const std::string& installer,
                      std::function<void(GameActionResult)> callback) {
  async::Run(
      context, [id, interactive, installer] { return InstallGameSync(id, interactive, installer); },
      std::move(callback));
}

void GetInstallProgressAsync(QObject* context, const std::string& id,
                             std::function<void(InstallProgressResult)> callback) {
  async::Run(context, [id] { return GetInstallProgressSync(id); }, std::move(callback));
}

void RelocateGameAsync(QObject* context, const std::string& id, const std::string& install_path,
                       const std::string& data_dir,
                       std::function<void(GameDetailResult)> callback) {
  json body = json::object();
  if (!install_path.empty()) body["install_path"] = install_path;
  if (!data_dir.empty()) body["data_dir"] = data_dir;
  RunJob<GameDetailResult>(
      context, "relocate",
      [id, body](const std::string& query) {
        return transport::PostJson("/v1/games/" + PercentEncode(id) + "/relocate" + query, body);
      },
      [id](GameDetailResult& result, const json& body) {
        if (!body.is_object())
          throw std::runtime_error(
              transport::UnexpectedResponse("POST /v1/games/" + id + "/relocate"));
        result.game = mapping::ToGameDetail(body);
      },
      std::move(callback));
}

void DeleteGamesAsync(QObject* context, const std::vector<std::string>& ids, bool delete_files,
                      bool delete_prefix, bool delete_metadata,
                      std::function<void(DeleteGamesResult)> callback) {
  const json body = {{"ids", ids},
                     {"delete_files", delete_files},
                     {"delete_prefix", delete_prefix},
                     {"delete_metadata", delete_metadata}};
  RunJob<DeleteGamesResult>(
      context, "delete",
      [body](const std::string& query) {
        return transport::PostJson("/v1/games/delete" + query, body);
      },
      FillDeleteGames, std::move(callback));
}

void DeleteInstallerAsync(QObject* context, const std::string& id,
                          std::function<void(GameActionResult)> callback) {
  async::Run(context, [id] { return DeleteInstallerSync(id); }, std::move(callback));
}

}  // namespace mira_gui::api
