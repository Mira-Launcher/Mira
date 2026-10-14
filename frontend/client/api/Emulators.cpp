#include "Emulators.h"

#include <json.hpp>

#include "../Async.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
namespace {

using nlohmann::json;

std::string EmulatorPath(const std::string& id, const std::string& action) {
  return "/v1/emulators/" + PercentEncode(id) + "/" + action;
}

EmulatorsResult GetEmulatorsSync() {
  return ReadReply<EmulatorsResult>(
      transport::Get("/v1/emulators"), "GET /v1/emulators", Shape::Array, [](EmulatorsResult& result, const json& body) {
        for (const json& entry : body) {
          EmulatorInfo info;
          info.id = entry.value("id", std::string());
          info.name = entry.value("name", std::string());
          info.extensions = entry.value("extensions", std::vector<std::string>());
          info.folder_games = entry.value("folder_games", false);
          info.installed = entry.value("installed", false);
          info.path = entry.value("path", std::string());
          info.version = entry.value("version", std::string());
          info.package = entry.value("package", std::string());
          info.rom_dir = entry.value("rom_dir", std::string());
          for (const json& item : entry.value("firmware", json::array())) {
            info.firmware.push_back({.id = item.value("id", std::string()),
                                     .label = item.value("label", std::string()),
                                     .required = item.value("required", true),
                                     .present = item.value("present", false)});
          }
          result.emulators.push_back(std::move(info));
        }
      });
}

StoreActionResult PostAction(const std::string& path, const json& body) {
  const transport::Reply reply = transport::PostJson(path, body);
  return {reply.ok, reply.error};
}

}  // namespace

void GetEmulatorsAsync(QObject* context, std::function<void(EmulatorsResult)> callback) {
  async::Run(context, [] { return GetEmulatorsSync(); }, std::move(callback));
}

void InstallEmulatorAsync(QObject* context, const std::string& id, std::function<void(StoreActionResult)> callback) {
  RunJob<StoreActionResult>(
      context, "install", [id](const std::string& query) { return transport::Post(EmulatorPath(id, "install") + query); },
      [](StoreActionResult&, const json&) {}, std::move(callback));
}

void AddEmulatorFilesAsync(QObject* context, const std::string& id, const std::vector<std::string>& paths,
                           std::function<void(StoreImportResult)> callback) {
  const json body = {{"paths", paths}};
  RunJob<StoreImportResult>(
      context, "import",
      [id, body](const std::string& query) { return transport::PostJson(EmulatorPath(id, "files") + query, body); },
      [](StoreImportResult& result, const json& reply) {
        FillAddedUpdated(result, reply);
        for (const json& item : reply.value("choose", json::array())) {
          result.choose.push_back({item.value("id", ""), item.value("name", ""),
                                   item.value("files", std::vector<std::string>{})});
        }
      },
      std::move(callback));
}

void ImportEmulatorAsync(QObject* context, const std::string& id, std::function<void(StoreImportResult)> callback) {
  RunJob<StoreImportResult>(
      context, "import", [id](const std::string& query) { return transport::Post(EmulatorPath(id, "import") + query); },
      FillAddedUpdated<StoreImportResult>, std::move(callback));
}

void AddEmulatorFirmwareAsync(QObject* context, const std::string& id, const std::string& firmware,
                              const std::vector<std::string>& paths, bool replace,
                              std::function<void(StoreActionResult)> callback) {
  const std::string path = EmulatorPath(id, "firmware/" + PercentEncode(firmware));
  async::Run(context, [path, paths, replace] { return PostAction(path, {{"paths", paths}, {"replace", replace}}); },
             std::move(callback));
}

void GetExistingEmulatorGamesAsync(QObject* context, const std::string& id,
                                   std::function<void(ExistingEmulatorGamesResult)> callback) {
  const std::string path = EmulatorPath(id, "existing");
  async::Run(context, [path] {
    return ReadReply<ExistingEmulatorGamesResult>(
        transport::Get(path), "GET " + path, Shape::Object, [](ExistingEmulatorGamesResult& result, const json& body) {
          for (const json& dir : body.value("dirs", json::array()))
            result.dirs.emplace_back(dir.value("path", std::string()), dir.value("games", 0));
          for (const json& shortcut : body.value("shortcuts", json::array()))
            result.shortcuts.push_back(shortcut.value("name", std::string()));
        });
  }, std::move(callback));
}

void AdoptEmulatorGamesAsync(QObject* context, const std::string& id, bool move,
                             std::function<void(StoreImportResult)> callback) {
  const json body = {{"move", move}};
  RunJob<StoreImportResult>(
      context, "import",
      [id, body](const std::string& query) { return transport::PostJson(EmulatorPath(id, "existing") + query, body); },
      FillAddedUpdated<StoreImportResult>, std::move(callback));
}

void OpenEmulatorAsync(QObject* context, const std::string& id, std::function<void(StoreActionResult)> callback) {
  const std::string path = EmulatorPath(id, "open");
  async::Run(context, [path] { return PostAction(path, json::object()); }, std::move(callback));
}

void GetGameEmulatorsAsync(QObject* context, const std::string& game_id,
                           std::function<void(EmulatorChoicesResult)> callback) {
  const std::string path = "/v1/games/" + PercentEncode(game_id) + "/emulators";
  async::Run(
      context,
      [path] {
        return ReadReply<EmulatorChoicesResult>(transport::Get(path), "GET " + path, Shape::Array,
                                                [](EmulatorChoicesResult& result, const json& body) {
                                                  for (const json& entry : body) {
                                                    result.emulators.push_back({entry.value("id", std::string()),
                                                                                entry.value("name", std::string())});
                                                  }
                                                });
      },
      std::move(callback));
}

void MoveGameToEmulatorAsync(QObject* context, const std::string& game_id, const std::string& emulator,
                             std::function<void(StoreActionResult)> callback) {
  const std::string path = "/v1/games/" + PercentEncode(game_id) + "/emulator";
  async::Run(context, [path, emulator] { return PostAction(path, {{"emulator", emulator}}); }, std::move(callback));
}

}  // namespace mira_gui::api
