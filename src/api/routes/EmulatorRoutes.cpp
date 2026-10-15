#include <httplib.h>

#include <format>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "emulators/Emulators.h"
#include "runner/EmulatorRunner.h"
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
          {"folder_games", emulators::ByFolder(emulator)},
          {"installed", !binary.empty()},
          {"path", binary.string()},
          {"version", emulators::InstalledVersion(config, emulator)},
          {"package", emulators::Downloads(emulator) ? "" : emulator.id},
          {"rom_dir", emulators::RomDir(config, emulator).string()},
          {"firmware", firmware}};
}

// The emulator the path names, or null after a 404.
const emulators::Emulator* FindEmulator(const Request& req, Response& res) {
  const emulators::Emulator* emulator = emulators::Find(req.matches[1].str());
  if (!emulator) SendError(res, 404, "emulator_not_found", "no such emulator");
  return emulator;
}

// The body's "paths", or empty after a 400.
std::vector<fs::path> PathsOf(const Request& req, Response& res) {
  const auto body = BodyObject(req, res, "{paths: [...]}");
  if (!body) return {};
  const auto paths = StringList(*body, "paths");
  if (!paths || paths->empty()) {
    SendError(res, 400, "no_paths", "paths must list at least one file");
    return {};
  }
  std::vector<fs::path> normal;
  for (const std::string& path : *paths) {
    if (!fs::path(path).is_absolute()) {
      SendError(res, 400, "invalid_body", "paths must be absolute");
      return {};
    }
    std::error_code ec;
    const fs::path source = fs::path(path).lexically_normal();
    if (!fs::is_regular_file(source, ec) && !fs::is_directory(source, ec)) {
      SendError(res, 400, "invalid_body", "paths must name existing files or folders");
      return {};
    }
    // "/x/Game/" names the folder Game: with no filename it would land on the ROM folder itself.
    const fs::path p(path);
    normal.push_back(p.has_filename() ? p : p.parent_path());
  }
  return normal;
}

void SourceAdded(Services& s, const emulators::Emulator& emulator, bool has_games) {
  s.games.MarkSourceAdded(emulator.id, has_games);
  s.events.Publish("sources.changed", json::object());
  if (s.on_roots_changed) s.on_roots_changed();
}

}  // namespace

void RegisterEmulatorRoutes(Router& http, Services& s) {
  http.Get("/v1/emulators", [&s](const Request&, Response& res) {
    json list = json::array();
    for (const emulators::Emulator& emulator : emulators::All())
      list.push_back(ToJson(s.config, emulator));
    SendJson(res, list);
  });

  http.Post(R"(/v1/emulators/([^/]+)/install)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = FindEmulator(req, res);
    if (!emulator) return;
    s.StartJob(req, res, "install", emulator->id, "Installing " + emulator->name,
               [&s, emulator](JobRegistry::Progress&) -> Result<json> {
                 const auto installed = emulators::Install(s.config, *emulator);
                 if (!installed) return std::unexpected(installed.error());
                 SourceAdded(s, *emulator, false);
                 return ToJson(s.config, *emulator);
               });
  });

  http.Post(R"(/v1/emulators/([^/]+)/import)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = FindEmulator(req, res);
    if (!emulator) return;
    s.StartJob(req, res, "import", emulator->id, "Importing " + emulator->name + " games",
               [&s, emulator](JobRegistry::Progress&) -> Result<json> {
                 const auto summary = emulators::Import(s.config, s.games, s.events, *emulator);
                 if (!summary) return std::unexpected(summary.error());
                 SourceAdded(s, *emulator, true);
                 s.AfterImport(summary->added_games);
                 return json{{"added", summary->added}, {"updated", summary->updated}};
               });
  });

  http.Post(R"(/v1/emulators/([^/]+)/files)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = FindEmulator(req, res);
    if (!emulator) return;
    std::vector<fs::path> files = PathsOf(req, res);
    if (files.empty()) return;
    const bool move =
        json::parse(req.body).value("move", emulators::MovesAdded(s.config, *emulator));
    s.StartJob(
        req, res, "import", emulator->id, "Adding " + emulator->name + " games",
        [&s, emulator, files = std::move(files), move](JobRegistry::Progress&) -> Result<json> {
          const auto summary =
              emulators::AddFiles(s.config, s.games, s.events, *emulator, files, move);
          if (!summary) return std::unexpected(summary.error());
          SourceAdded(s, *emulator, true);
          s.AfterImport(summary->added_games);
          json choose = json::array();
          for (const model::Game& game : summary->added_games) {
            if (auto files = emulators::StartChoices(s.config, game); !files.empty())
              choose.push_back({{"id", game.id}, {"name", game.name}, {"files", std::move(files)}});
          }
          return json{{"added", summary->added},
                      {"updated", summary->updated},
                      {"choose", std::move(choose)}};
        });
  });

  http.Post(R"(/v1/emulators/([^/]+)/firmware/([^/]+))", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = FindEmulator(req, res);
    if (!emulator) return;
    const auto firmware =
        std::ranges::find(emulator->firmware, req.matches[2].str(), &emulators::Firmware::id);
    if (firmware == emulator->firmware.end())
      return SendError(res, 404, "firmware_not_found", "no such firmware");
    const std::vector<fs::path> files = PathsOf(req, res);
    if (files.empty()) return;
    const json body = json::parse(req.body, nullptr, false);
    const bool replace = body.is_object() && body.value("replace", json(false)) == json(true);
    if (auto added = emulators::AddFirmware(s.config, *emulator, *firmware, files, replace); !added)
      return SendError(res, added.error().code == "already_there" ? 409 : 422, added.error());
    SendJson(res, ToJson(s.config, *emulator));
  });

  http.Post(R"(/v1/emulators/([^/]+)/open)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = FindEmulator(req, res);
    if (!emulator) return;
    const fs::path binary = emulators::Binary(s.config, *emulator);
    if (binary.empty())
      return SendError(res, 409, "emulator_missing", emulator->name + " isn't installed");
    Command command = runner::EmulatorCommand(
        emulator->open_binary.empty() ? binary : binary.parent_path() / emulator->open_binary,
        *emulator);
    std::ranges::copy(emulator->open_args, std::back_inserter(command.argv));
    if (auto spawned = runner::SpawnReaped(command); !spawned)
      return SendError(res, 500, spawned.error());
    SendJson(res, {{"status", "opened"}});
  });

  http.Get(R"(/v1/emulators/([^/]+)/existing)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = FindEmulator(req, res);
    if (!emulator) return;
    const emulators::ExistingGames existing = emulators::FindExisting(s.config, s.games, *emulator);
    json dirs = json::array();
    for (const auto& dir : existing.dirs)
      dirs.push_back({{"path", dir.path.string()}, {"games", dir.games}});
    json shortcuts = json::array();
    for (const auto& shortcut : existing.shortcuts)
      shortcuts.push_back(
          {{"id", shortcut.id}, {"name", shortcut.name}, {"file", shortcut.file.string()}});
    SendJson(res, {{"dirs", dirs}, {"shortcuts", shortcuts}});
  });

  http.Post(R"(/v1/emulators/([^/]+)/existing)", [&s](const Request& req, Response& res) {
    const emulators::Emulator* emulator = FindEmulator(req, res);
    if (!emulator) return;
    const auto body = BodyObject(req, res, "{move}");
    if (!body) return;
    const bool move = body->value("move", emulators::MovesAdded(s.config, *emulator));
    s.StartJob(req, res, "import", emulator->id, "Adding " + emulator->name + "'s games",
               [&s, emulator, move](JobRegistry::Progress&) -> Result<json> {
                 const auto summary =
                     emulators::AdoptExisting(s.config, s.games, s.events, *emulator, move);
                 if (!summary) return std::unexpected(summary.error());
                 SourceAdded(s, *emulator, true);
                 s.AfterImport(summary->added_games);
                 return json{{"added", summary->added}, {"updated", summary->updated}};
               });
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
    const auto body = BodyObject(req, res, "{emulator}");
    if (!body) return;
    const emulators::Emulator* to = emulators::Find(body->value("emulator", std::string()));
    if (!to) return SendError(res, 404, "emulator_not_found", "no such emulator");
    const auto folders_lock = s.games.LockFolders();
    // No launch, move or delete while its files move.
    const auto claim = s.Claim(game->id, "moved");
    if (!claim) return SendError(res, 409, claim.error());
    const auto current = s.games.Find(game->id);  // as it is now that it's claimed
    if (!current) return SendError(res, 404, "game_not_found", "no such game");
    const auto moved = emulators::MoveGame(s.config, s.games, *current, *to);
    if (!moved) return SendError(res, 409, moved.error());
    const json record = s.Record(*moved);
    s.events.Publish("game.updated", record);
    SourceAdded(s, *to, true);
    SendJson(res, record);
  });
}

}  // namespace mira::api
