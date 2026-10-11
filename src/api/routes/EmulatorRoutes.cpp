#include "api/Routes.h"

#include <format>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "emulators/Emulators.h"
#include "runner/Exec.h"

namespace mira::api {
namespace {
namespace fs = std::filesystem;
using httplib::Request;
using httplib::Response;
using nlohmann::json;

json ToJson(const config::Config& config, const emulators::Emulator& emulator) {
  json firmware = json::array();
  for (const emulators::FirmwareState& state : emulators::FirmwareStatus(emulator)) {
    firmware.push_back({{"id", state.firmware->id},
                        {"label", state.firmware->label},
                        {"required", state.firmware->required},
                        {"present", state.present}});
  }
  const fs::path binary = emulators::Binary(config, emulator);
  return {{"id", emulator.id},
          {"name", emulator.name},
          {"consoles", emulator.consoles},
          {"extensions", emulator.extensions},
          {"folder_games", !emulator.folder_game.empty()},
          {"installed", !binary.empty()},
          {"path", binary.string()},
          {"version", emulators::InstalledVersion(config, emulator)},
          {"package", emulators::Downloads(emulator) ? "" : emulator.id},
          {"rom_dir", emulators::RomDir(config, emulator).string()},
          {"firmware", firmware}};
}

// The body's "paths" as paths; empty when it has none.
std::vector<fs::path> PathsOf(const json& body) {
  std::vector<fs::path> paths;
  if (!body.contains("paths") || !body["paths"].is_array()) return paths;
  for (const json& path : body["paths"]) {
    if (path.is_string()) paths.emplace_back(path.get<std::string>());
  }
  return paths;
}

}  // namespace

void RegisterEmulatorRoutes(httplib::Server& http, Services& s) {
  http.Get("/v1/emulators", [&s](const Request&, Response& res) {
    json list = json::array();
    for (const emulators::Emulator& emulator : emulators::All()) list.push_back(ToJson(s.config, emulator));
    SendJson(res, list);
  });

  http.Post(R"(/v1/emulators/([^/]+)/install)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = emulators::Find(req.matches[1].str());
    if (!emulator) return SendError(res, 404, "emulator_not_found", "no such emulator");
    s.StartJob(req, res, "install", emulator->id, "Installing " + emulator->name,
               [&s, emulator](JobRegistry::Progress&) -> Result<json> {
                 const auto installed = emulators::Install(s.config, *emulator);
                 if (!installed) return std::unexpected(installed.error());
                 s.games.MarkSourceAdded(emulator->id, false);
                 s.events.Publish("sources.changed", json::object());
                 if (s.on_roots_changed) s.on_roots_changed();
                 return ToJson(s.config, *emulator);
               });
  });

  http.Post(R"(/v1/emulators/([^/]+)/import)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = emulators::Find(req.matches[1].str());
    if (!emulator) return SendError(res, 404, "emulator_not_found", "no such emulator");
    s.StartJob(req, res, "import", emulator->id, "Importing " + emulator->name + " games",
               [&s, emulator](JobRegistry::Progress&) -> Result<json> {
                 const auto summary = emulators::Import(s.config, s.games, s.events, *emulator);
                 if (!summary) return std::unexpected(summary.error());
                 s.games.MarkSourceAdded(emulator->id, true);
                 s.events.Publish("sources.changed", json::object());
                 if (s.on_roots_changed) s.on_roots_changed();
                 s.AfterImport(summary->added_games);
                 return json{{"added", summary->added}, {"updated", summary->updated}};
               });
  });

  http.Post(R"(/v1/emulators/([^/]+)/files)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = emulators::Find(req.matches[1].str());
    if (!emulator) return SendError(res, 404, "emulator_not_found", "no such emulator");
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return SendError(res, 400, "invalid_body", "expected a JSON object");
    std::vector<fs::path> files = PathsOf(body);
    if (files.empty()) return SendError(res, 400, "no_paths", "paths must list at least one file");
    const bool move = body.value("move", s.config.GetBool("emulators.move_added"));
    s.StartJob(req, res, "import", emulator->id, "Adding " + emulator->name + " games",
               [&s, emulator, files = std::move(files), move](JobRegistry::Progress&) -> Result<json> {
                 const auto summary = emulators::AddFiles(s.config, s.games, s.events, *emulator, files, move);
                 if (!summary) return std::unexpected(summary.error());
                 s.games.MarkSourceAdded(emulator->id, true);
                 s.events.Publish("sources.changed", json::object());
                 if (s.on_roots_changed) s.on_roots_changed();
                 s.AfterImport(summary->added_games);
                 return json{{"added", summary->added}, {"updated", summary->updated}};
               });
  });

  http.Post(R"(/v1/emulators/([^/]+)/firmware/([^/]+))", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = emulators::Find(req.matches[1].str());
    if (!emulator) return SendError(res, 404, "emulator_not_found", "no such emulator");
    const auto firmware = std::ranges::find(emulator->firmware, req.matches[2].str(), &emulators::Firmware::id);
    if (firmware == emulator->firmware.end()) return SendError(res, 404, "firmware_not_found", "no such firmware");
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return SendError(res, 400, "invalid_body", "expected a JSON object");
    const std::vector<fs::path> files = PathsOf(body);
    if (files.empty()) return SendError(res, 400, "no_paths", "paths must list at least one file");
    if (auto added = emulators::AddFirmware(s.config, *emulator, *firmware, files); !added)
      return SendError(res, 422, added.error());
    SendJson(res, ToJson(s.config, *emulator));
  });

  http.Post(R"(/v1/emulators/([^/]+)/open)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = emulators::Find(req.matches[1].str());
    if (!emulator) return SendError(res, 404, "emulator_not_found", "no such emulator");
    const fs::path binary = emulators::Binary(s.config, *emulator);
    if (binary.empty()) return SendError(res, 409, "emulator_missing", emulator->name + " isn't installed");
    emulators::Prefill(*emulator);
    Command command;
    command.argv = {binary.string()};
    command.argv.insert(command.argv.end(), emulator->open_args.begin(), emulator->open_args.end());
    for (const auto& [key, value] : emulators::Environment(*emulator)) command.env[key] = value;
    if (auto spawned = runner::SpawnDetached(command); !spawned) return SendError(res, 500, spawned.error());
    SendJson(res, {{"status", "opened"}});
  });

  http.Get(R"(/v1/games/([^/]+)/emulators)", [&s](const Request& req, Response& res) {
    const auto game = s.games.Find(req.matches[1].str());
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    json list = json::array();
    for (const emulators::Emulator* emulator : emulators::Alternatives(s.config, *game))
      list.push_back({{"id", emulator->id}, {"name", emulator->name}});
    SendJson(res, list);
  });

  http.Post(R"(/v1/games/([^/]+)/emulator)", [&s](const Request& req, Response& res) {
    const auto game = s.games.Find(req.matches[1].str());
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return SendError(res, 400, "invalid_body", "expected a JSON object");
    const emulators::Emulator* to = emulators::Find(body.value("emulator", std::string()));
    if (!to) return SendError(res, 404, "emulator_not_found", "no such emulator");
    const auto moved = emulators::MoveGame(s.config, s.games, *game, *to);
    if (!moved) return SendError(res, 409, moved.error());
    s.games.MarkSourceAdded(to->id, true);
    if (s.on_roots_changed) s.on_roots_changed();
    s.events.Publish("game.updated", model::ToJson(*moved));
    s.events.Publish("sources.changed", json::object());
    SendJson(res, model::ToJson(*moved));
  });
}

}  // namespace mira::api
