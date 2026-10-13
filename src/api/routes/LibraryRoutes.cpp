#include <httplib.h>

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <set>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "emulators/Emulators.h"
#include "modmanagers/ModManagers.h"
#include "core/Command.h"
#include "core/Log.h"
#include "desktop/DesktopEntryScanner.h"
#include "library/Catalog.h"
#include "library/FolderTags.h"
#include "library/Import.h"
#include "library/Relocate.h"
#include "library/Scanner.h"
#include "library/SourceRegistry.h"
#include "library/SourceRemoval.h"
#include "library/SourceRunner.h"
#include "lutris/LutrisImporter.h"
#include "metadata/MetadataFetcher.h"
#include "steam/FriendsStatus.h"
#include "runner/Exec.h"
#include "steam/Shortcuts.h"
#include "steam/SteamDetector.h"
#include "steam/SteamScanner.h"

namespace mira::api {
namespace {
using httplib::Request;

// Known sources, in default order.
const std::vector<std::string> kSourceIds = [] {
  std::vector<std::string> ids = {"local", "steam",  "epic",    "gog",   "itch", "amazon",
                                  "humble", "battlenet", "ubisoft", "ea", "office", "adobe", "lutris"};
  for (const emulators::Emulator& emulator : emulators::All()) ids.push_back(emulator.id);
  for (const modmanagers::ModManager& manager : modmanagers::All()) ids.push_back(manager.id);
  return ids;
}();

using httplib::Response;
using nlohmann::json;

}  // namespace

void RegisterLibraryRoutes(Router& http, Services& s) {
  // --- library ------------------------------------------------------------

  http.Post("/v1/library/scan", [&s](const Request& req, Response& res) {
    s.StartJob(req, res, "scan", "", "Scanning your library", [&s](JobRegistry::Progress&) -> Result<json> {
      const library::ScanSummary summary = s.Scan();
      return json{{"added", summary.added},
                  {"missing", summary.missing},
                  {"restored", summary.restored},
                  {"moved", summary.moved}};
    });
  });

  http.Get("/v1/library/unclear", [&s](const Request&, Response& res) {
    json moves = json::array();
    for (const library::UnclearMove& move : s.unclear_moves.All()) {
      json entry = library::UnclearMoves::ToJson(move, s.games);
      json games = json::array();
      for (json& game : entry["games"]) {
        if (!s.Conceals(game.value("id", std::string()))) games.push_back(std::move(game));
      }
      if (games.empty()) continue;
      entry["games"] = std::move(games);
      moves.push_back(std::move(entry));
    }
    SendJson(res, {{"moves", std::move(moves)}});
  });

  http.Post("/v1/library/unclear", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"folder": "...", "id"?: "..."})";
    const auto parsed = BodyObject(req, res, kShape);
    if (!parsed) return;
    const json& body = *parsed;
    if (!body.contains("folder") || !body["folder"].is_string() ||
        (body.contains("id") && !body["id"].is_string() && !body["id"].is_null())) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const auto move = s.unclear_moves.Find(body["folder"].get<std::string>());
    if (!move) return SendError(res, 404, "move_not_found", "no unclear move for that folder");
    std::optional<std::string> id;
    if (body.contains("id") && body["id"].is_string()) {
      id = body["id"].get<std::string>();
      if (!std::ranges::contains(move->ids, *id)) {
        return SendError(res, 400, "invalid_body", "that game isn't one this folder could be");
      }
    }
    library::Scanner scanner(s.config, s.games, s.events);
    scanner.UseUnclearMoves(s.unclear_moves);
    auto settled = scanner.Settle(move->folder, id);
    if (!settled) return SendError(res, 409, settled.error());
    s.SyncDesktopEntry(settled->id);
    if (!id) {
      s.QueueMetadata({*settled});
      // Set up like a game a scan adds.
      if (settled->status == model::GameStatus::SettingUp && s.config.GetBool("auto_setup"))
        s.ProvisionLater(*settled);
    }
    SendJson(res, s.Record(*settled));
  });

  // One game at a time, since a move can copy a whole game. Each moved game
  // publishes its own game.updated.
  http.Post("/v1/library/relocate", [&s](const Request& req, Response& res) {
    const json body = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
    const auto ids = body.is_object() ? StringList(body, "ids") : std::nullopt;
    if (!ids) return SendError(res, 400, "invalid_body", R"(expected no body, or {"ids": [...]})");

    std::vector<model::Game> games = s.games.All();
    if (body.contains("ids")) {
      std::erase_if(games, [&](const model::Game& game) { return !std::ranges::contains(*ids, game.id); });
    }
    s.StartJob(req, res, "relocate", "", "Moving games into Mira's folders",
             [&s, games = std::move(games)](JobRegistry::Progress& progress) -> Result<json> {
               int moved = 0;
               std::vector<std::string> moved_ids;
               int done = 0;
               json errors = json::array();
               for (const model::Game& listed : games) {
                 progress.Report(done++, static_cast<int>(games.size()), listed.name);
                 // Per game, so scans can run between moves.
                 auto folders_lock = s.games.LockFolders();
                 // Read again now: earlier moves take time, and the game may have changed
                 // meanwhile.
                 const auto current = s.games.Find(listed.id);
                 if (!current) continue;  // removed meanwhile
                 const model::Game& game = *current;
                 // Held for the whole move, so the game can't launch from half-moved files.
                 const auto claim = s.Claim(game.id, "moved");
                 if (!claim) {
                   errors.push_back(BatchFailure(game.id, claim.error()));
                   continue;
                 }
                 auto relocated = library::Relocate(s.config, game, {}, s.games.All());
                 if (!relocated) {
                   log::Warn("relocate failed for {}: {}", game.id, relocated.error().message);
                   errors.push_back(BatchFailure(game.id, relocated.error()));
                   continue;
                 }
                 if (relocated->install_path == game.install_path && relocated->data_dir == game.data_dir &&
                     relocated->exe_path == game.exe_path) {
                   continue;
                 }
                 auto saved = s.games.Update(game.id, [&](model::Game& g) {
                   g.install_path = relocated->install_path;
                   g.exe_path = relocated->exe_path;
                   g.data_dir = relocated->data_dir;
                   g.source = relocated->source;
                   g.source_ref = relocated->source_ref;
                   g.updated_at = model::NowSeconds();
                 });
                 if (!saved) {
                   errors.push_back(BatchFailure(game.id, saved.error()));
                   continue;
                 }
                 library::PruneEmptyContainers(s.config, library::SortingFolderOf(s.config, game),
                                               s.games.All());
                 s.events.Publish("game.updated", s.Record(*saved));
                 moved_ids.push_back(game.id);
                 ++moved;
               }
               s.SyncDesktopEntries();
               // A game sorted by a link needs its link pointed at its new folder.
               s.SortByTags(std::move(moved_ids));
               const int failed = static_cast<int>(errors.size());
               return json{{"moved", moved}, {"failed", failed}, {"errors", std::move(errors)}};
             });
  });

  // --- import ------------------------------------------------------------

  http.Post("/v1/library/import/classify", [](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"path": "/absolute/path"})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const std::string path = body->contains("path") && (*body)["path"].is_string() ? (*body)["path"].get<std::string>() : "";
    if (!std::filesystem::path(path).is_absolute()) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return SendError(res, 404, "not_found", path + " doesn't exist");
    const library::ImportGuess guess = library::ClassifyImport(path);
    SendJson(res, json{{"kind", std::string(library::ImportKindName(guess.kind))}, {"name", guess.name},
                       {"reason", guess.reason}});
  });

  http.Post("/v1/library/import", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"path": "/absolute/path", "kind": "game" or "app"})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const std::string path = body->contains("path") && (*body)["path"].is_string() ? (*body)["path"].get<std::string>() : "";
    const std::optional<library::ImportKind> kind =
        body->contains("kind") && (*body)["kind"].is_string() ? library::ParseImportKind((*body)["kind"].get<std::string>()) : std::nullopt;
    if (!std::filesystem::path(path).is_absolute() || !kind) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    s.StartJob(req, res, "import_path", path, "Adding " + std::filesystem::path(path).filename().string(),
               [&s, path, kind = *kind](JobRegistry::Progress&) -> Result<json> {
                 const Result<std::filesystem::path> target = library::ImportInto(s.config, path, kind);
                 if (!target) return std::unexpected(target.error());
                 return json{{"path", target->string()}};
               });
  });

  // --- steam ------------------------------------------------------------

  http.Post("/v1/steam/scan", [&s](const Request& req, Response& res) {
    s.StartJob(req, res, "import", "steam", "Importing from Steam", [&s](JobRegistry::Progress&) -> Result<json> {
      steam::SteamScanner scanner(s.config, s.games, s.events);
      auto summary = scanner.Scan();
      if (!summary) return std::unexpected(summary.error());
      s.games.MarkSourceAdded("steam", true);
      s.events.Publish("sources.changed", json::object());
      s.AfterImport(summary->added_games);
      return json{{"added", summary->added}, {"updated", summary->updated}};
    });
  });

  http.Get("/v1/steam/accounts", [&s](const Request&, Response& res) {
    json accounts = json::array();
    std::string selected = s.config.GetString("steam.steamid64");
    const auto root = steam::FindSteamRoot(s.config);
    if (root) {
      for (const steam::SteamAccount& account : steam::Accounts(*root)) {
        accounts.push_back({{"steamid64", account.steamid64},
                            {"account_name", account.account_name},
                            {"persona_name", account.persona_name},
                            {"most_recent", account.most_recent}});
      }
    }
    if (selected.empty() && !accounts.empty()) selected = accounts.front()["steamid64"];
    SendJson(res, {{"found", root.has_value()}, {"accounts", accounts}, {"selected", selected}});
  });

  http.Get("/v1/steam/installed", [&s](const Request&, Response& res) {
    const auto root = steam::FindSteamRoot(s.config);
    if (!root) return SendJson(res, {{"found", false}, {"games", json::array()}});
    const auto activity = steam::ReadAppActivity(*root, s.config.GetString("steam.steamid64"));
    const auto played = [&activity](const std::string& appid) {
      const auto it = activity.find(appid);
      return it == activity.end() ? std::int64_t{0} : it->second.last_played_at;
    };
    std::vector<steam::SteamApp> apps = steam::ListApps(*root);
    std::ranges::stable_sort(apps, std::greater{},
                             [&played](const steam::SteamApp& app) { return played(app.appid); });
    json games = json::array();
    for (const steam::SteamApp& app : apps) {
      if (s.Conceals("steam-" + app.appid)) continue;
      json game = {{"appid", app.appid}, {"name", app.name}};
      if (const std::int64_t at = played(app.appid); at > 0) game["last_played_at"] = at;
      games.push_back(std::move(game));
    }
    SendJson(res, {{"found", true}, {"games", games}});
  });

  http.Post("/v1/steam/status", [&s](const Request& req, Response& res) {
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object())
      return SendError(res, 400, "invalid_body", "expected a JSON object");
    const auto set = steam::SetFriendsStatus(body.value("status", std::string()), steam::SteamPidFile(s.config),
                                             steam::SteamCommand(s.config));
    if (!set) return SendError(res, set.error().code == "invalid_status" ? 400 : 409, set.error());
    SendJson(res, {{"status", body.value("status", std::string())}});
  });

  http.Post("/v1/steam/shortcut", [&s](const Request& req, Response& res) {
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("exe"))
      return SendError(res, 400, "invalid_body", "expected {\"exe\", \"launch_options\"}");
    if (!s.config.GetBool("steam.mira_shortcut")) return SendJson(res, {{"status", "disabled"}});
    const auto root = steam::FindSteamRoot(s.config);
    if (!root) return SendError(res, 404, "steam_not_found", "Steam isn't installed");
    const auto change = steam::EnsureShortcut(*root, "Mira", body.value("exe", std::string()),
                                              body.value("launch_options", std::string()));
    if (!change) return SendError(res, 500, change.error());
    SendJson(res, {{"status", "ok"}, {"added", change->added}, {"updated", change->updated}});
  });

  http.Post("/v1/steam/bigpicture", [&s](const Request&, Response& res) {
    Command command;
    command.argv = steam::SteamCommand(s.config);
    command.argv.push_back("steam://open/bigpicture");
    if (auto spawned = runner::SpawnDetached(command); !spawned) return SendError(res, 500, spawned.error());
    SendJson(res, {{"status", "opened"}});
  });

  // --- lutris -----------------------------------------------------------

  http.Get("/v1/lutris", [&s](const Request&, Response& res) {
    const auto dir = lutris::FindLutrisDataDir(s.config);
    SendJson(res, {{"found", dir.has_value()}, {"data_dir", dir ? dir->string() : ""}});
  });

  http.Post("/v1/lutris/import", [&s](const Request& req, Response& res) {
    s.StartJob(req, res, "import", "lutris", "Importing from Lutris", [&s](JobRegistry::Progress&) -> Result<json> {
      lutris::LutrisImporter importer(s.config, s.games, s.events);
      auto summary = importer.Import();
      if (!summary) return std::unexpected(summary.error());
      s.games.MarkSourceAdded("lutris", true);
      s.events.Publish("sources.changed", json::object());
      s.AfterImport(summary->added_games);
      return json{{"added", summary->added},
                  {"updated", summary->updated},
                  {"other_runner", summary->other_runner},
                  {"incomplete", summary->incomplete}};
    });
  });

  // --- sources --------------------------------------------------------------

  http.Get(R"(/v1/sources/([a-z0-9-]+)/removal)", [&s](const Request& req, Response& res) {
    auto plan = library::PlanRemoval(s.config, s.games, req.matches[1].str());
    if (!plan) return SendError(res, 404, plan.error());
    json games = json::array();
    for (const library::RemovalGame& game : plan->games) {
      games.push_back({{"id", game.id}, {"name", game.name}, {"deletes", game.deletes}});
    }
    SendJson(res, {{"source", plan->source},
                   {"games", games},
                   {"program_dir", plan->program_dir},
                   {"packages", plan->packages},
                   {"uninstall", plan->uninstall},
                   {"data", plan->data},
                   {"kept", plan->kept},
                   {"signs_out", plan->signs_out}});
  });

  http.Post(R"(/v1/sources/([a-z0-9-]+)/remove)", [&s](const Request& req, Response& res) {
    const std::string source = req.matches[1].str();
    // Checked now, so an unknown source is a 404 rather than a failed job.
    if (auto plan = library::PlanRemoval(s.config, s.games, source); !plan) return SendError(res, 404, plan.error());
    const json body = json::parse(req.body, nullptr, false);
    const bool delete_data = body.is_object() && body.value("delete_data", false);
    s.StartJob(req, res, "remove_source", source, "Removing " + source,
             [&s, source, delete_data](JobRegistry::Progress&) -> Result<json> {
               auto removed = library::RemoveSource(s.config, s.games, s.events, source, delete_data);
               if (!removed) return std::unexpected(removed.error());
               s.SyncDesktopEntries();
               s.events.Publish("sources.changed", json::object());
               return json{{"removed", removed->removed}, {"problems", removed->problems}};
             });
  });

  const auto source_json = [](const std::map<std::string, int>& counts, const store::SourceState& state) {
    const auto found = counts.find(state.id);
    return json{{"id", state.id},
                {"added", state.added || state.id == "local"},
                {"enabled", state.enabled},
                {"in_sidebar", state.in_sidebar},
                {"position", state.position},
                {"imported_at", state.imported_at},
                {"games", found == counts.end() ? 0 : found->second}};
  };

  http.Get("/v1/sources", [&s, source_json](const Request&, Response& res) {
    std::map<std::string, store::SourceState, std::less<>> saved;
    for (store::SourceState& state : s.games.Sources()) saved.emplace(state.id, std::move(state));
    std::vector<store::SourceState> states;
    for (const std::string& id : kSourceIds) {
      const auto found = saved.find(id);
      states.push_back(found != saved.end() ? found->second : store::SourceState{.id = id});
    }
    // Never-ordered sources (position 0) go last, in default order.
    std::ranges::stable_sort(states, std::ranges::less{}, [](const store::SourceState& state) {
      return state.position == 0 ? std::numeric_limits<int>::max() : state.position;
    });
    const auto counts = s.games.CountBySource(s.hidden.Locked());
    json sources = json::array();
    for (const store::SourceState& state : states) sources.push_back(source_json(counts, state));
    SendJson(res, {{"sources", sources}});
  });

  http.Patch(R"(/v1/sources/([a-z0-9-]+))", [&s, source_json](const Request& req, Response& res) {
    const std::string id = req.matches[1].str();
    if (std::ranges::find(kSourceIds, id) == kSourceIds.end()) {
      return SendError(res, 404, "unknown_source", std::format("no source named \"{}\"", id));
    }
    const auto body = BodyObject(req, res, R"({"enabled": true, "in_sidebar": true})");
    if (!body) return;
    store::SourceState state = s.games.Source(id);
    const auto flag = [&body](const char* key, bool& field) {
      if (body->contains(key) && (*body)[key].is_boolean()) field = (*body)[key].get<bool>();
    };
    const bool was_added = state.added;
    flag("added", state.added);
    if (state.added && !was_added) state.enabled = true;  // setting a source up means wanting its games
    flag("enabled", state.enabled);
    flag("in_sidebar", state.in_sidebar);
    s.games.SaveSource(state);
    SendJson(res, source_json(s.games.CountBySource(s.hidden.Locked()), state));
    s.events.Publish("sources.changed", json::object());
  });

  http.Put("/v1/sources/order", [&s](const Request& req, Response& res) {
    const auto body = BodyObject(req, res, R"({"order": ["steam", "..."]})");
    if (!body) return;
    const auto order = StringList(*body, "order");
    if (!order || !body->contains("order")) return SendError(res, 400, "invalid_body", R"(expected {"order": [...]})");
    std::vector<std::string> ids;
    for (const std::string& id : *order) {
      if (std::ranges::find(kSourceIds, id) != kSourceIds.end()) ids.push_back(id);
    }
    s.games.SetSourceOrder(ids);
    SendJson(res, {{"ok", true}});
    s.events.Publish("sources.changed", json::object());
  });

  const auto send_source_runner = [](Response& res, const Result<library::SourceRunner>& runner) {
    if (!runner) {
      const int status = runner.error().code == "launcher_not_installed" ? 409 : 400;
      return SendError(res, status, runner.error());
    }
    SendJson(res, {{"runner_ref", runner->runner_ref}, {"games", runner->games}, {"differing", runner->differing}});
  };

  http.Get(R"(/v1/sources/([a-z0-9-]+)/runner)", [&s, send_source_runner](const Request& req, Response& res) {
    send_source_runner(res, library::GetSourceRunner(s.config, s.games, req.matches[1].str()));
  });

  http.Post(R"(/v1/sources/([a-z0-9-]+)/runner)", [&s, send_source_runner](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"runner_ref": "kind:name", "apply_to_games"?: bool})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const json& b = *body;
    if (!b.contains("runner_ref") || !b["runner_ref"].is_string()) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const auto runner = library::SetSourceRunner(s.config, s.games, req.matches[1].str(),
                                                 b["runner_ref"].get<std::string>(),
                                                 b.value("apply_to_games", false));
    if (runner) {
      for (const std::string& id : runner->changed) {
        if (const auto game = s.games.Find(id)) s.events.Publish("game.updated", s.Record(*game));
      }
    }
    send_source_runner(res, runner);
  });

  // --- library (what the account owns, across sources) ------------------

  http.Get("/v1/library", [&s](const Request& req, Response& res) {
    const std::string source = Param(req, "source");
    auto entries = s.catalogs.List(s.config, s.games, s.events, s.catalog_checks, source, BoolParam(req, "fresh"));
    if (!entries) return SendError(res, 400, entries.error());
    const auto steam_tags = metadata::StoredSteamTags(s.games.Metadata());
    const auto reviews = s.games.Metadata().Field("steam_reviews");
    const std::set<std::string> hidden_keys = s.games.HiddenKeys();
    json out = json::array();
    const bool locked = s.hidden.Locked();
    for (const library::CatalogEntry& entry : *entries) {
      const std::string key = entry.source + "-" + entry.ref;
      const bool hidden = hidden_keys.contains(key);
      if (locked && hidden) continue;
      out.push_back({{"source", entry.source},
                     {"ref", entry.ref},
                     {"title", entry.title},
                     {"installed", entry.installed},
                     {"game_id", entry.game_id},
                     {"play_seconds", entry.play_seconds},
                     {"owned", entry.owned},
                     {"hidden", hidden}});
      if (std::string tier = s.games.Metadata().ProtonDbTier(key); !tier.empty()) {
        out.back()["protondb_tier"] = std::move(tier);
      }
      if (const auto tags = steam_tags.find(key);
          tags != steam_tags.end() && tags->second && !tags->second->empty()) {
        out.back()["steam_tags"] = *tags->second;
      }
      if (const auto found = reviews.find(key);
          found != reviews.end() && found->second.is_object()) {
        const json& r = found->second;
        const std::int64_t total = r.value("total_reviews", std::int64_t{0});
        if (total > 0) {
          const std::int64_t positive = r.value("total_positive", std::int64_t{0});
          out.back()["steam_reviews"] = {{"score_description", r.value("score_description", std::string())},
                                         {"percent_positive", (positive * 100 + total / 2) / total},
                                         {"total_reviews", total}};
        }
      }
    }
    SendJson(res, std::move(out));
  });

  // Keyed by {source, ref}, since the title isn't tracked yet. "update" in the
  // event payload tells an update from an install.
  auto library_install_or_update = [&s](const Request& req, Response& res, bool is_update) {
    constexpr std::string_view kShape = R"({"source": "epic"|"steam"|"gog"|"itch"|"amazon", "ref": "..."})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const json& b = *body;
    if (!b.contains("source") || !b["source"].is_string() ||
        !b.contains("ref") || !b["ref"].is_string()) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const std::string source = b["source"];
    const std::string ref = b["ref"];

    library::ILibrarySource* src = library::FindSource(source);
    if (src == nullptr) {
      return SendError(res, 400, "unknown_source", std::format("no installable source named \"{}\"", source));
    }
    if (!IsSafeRef(ref)) return SendError(res, 400, "invalid_ref", "that ref isn't a store id");

    s.paused_installs.Remove(source + "-" + ref);  // installing again resumes it
    s.events.Publish("library.install.started", {{"source", source}, {"ref", ref}, {"update", is_update}});
    s.StartJob(req, res, is_update ? "update" : "install", source + "-" + ref, (is_update ? "Updating " : "Installing ") + ref,
               [&s, src, source, ref, is_update](JobRegistry::Progress&) -> Result<json> {
                 const Result<void> result = is_update ? src->Update(s.config, s.games, s.events, ref)
                                                       : src->Install(s.config, s.games, s.events, ref);
                 if (!result && s.paused_installs.Contains(source + "-" + ref)) {
                   log::Info("{} {} paused: {}", source, is_update ? "update" : "install", ref);
                   s.events.Publish("library.install.paused", {{"source", source}, {"ref", ref}, {"update", is_update}});
                   return std::unexpected(result.error());
                 }
                 if (!result) {
                   log::Error("{} {} failed ({}): {}", source, is_update ? "update" : "install", ref,
                              result.error().message);
                   s.events.Publish("library.install.failed",
                                    FailedEvent({{"source", source}, {"ref", ref}, {"update", is_update}}, result.error()));
                   return std::unexpected(result.error());
                 }
                 s.paused_installs.Remove(source + "-" + ref);  // finished before a pause took hold
                 log::Info("{} {} finished: {}", source, is_update ? "update" : "install", ref);
                 s.SyncDesktopEntry(source + "-" + ref);
                 if (const auto game = s.games.Find(source + "-" + ref)) s.fetches.Enqueue(s.config, s.events, *game);
                 s.events.Publish("library.install.finished", {{"source", source}, {"ref", ref}, {"update", is_update}});
                 return json{{"source", source}, {"ref", ref}};
               },
               &s.operations);
  };
  http.Get("/v1/library/artwork", [&s](const Request& req, Response& res) {
    const std::string source = Param(req, "source");
    const std::string ref = Param(req, "ref");
    if (library::FindSource(source) == nullptr || !IsSafeRef(ref)) {
      return SendError(res, 400, "invalid_request", "expected ?source=<store>&ref=<ref>");
    }
    SendCachedArtwork(s.games.Metadata(), source + "-" + ref, "cover", res);
  });

  http.Get("/v1/library/metadata", [&s](const Request& req, Response& res) {
    const std::string source = Param(req, "source");
    const std::string ref = Param(req, "ref");
    if (library::FindSource(source) == nullptr || !IsSafeRef(ref)) {
      return SendError(res, 400, "invalid_request", "expected ?source=<store>&ref=<ref>");
    }
    if (!s.games.Metadata().Has(source + "-" + ref)) {
      return SendError(res, 404, "metadata_not_found", "no metadata cached for this title yet");
    }
    SendJson(res, s.games.Metadata().Read(source + "-" + ref));
  });

  http.Post("/v1/library/artwork", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"source": "...", "titles": [{"ref": "...", "title": "..."}]})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const json& b = *body;
    const auto text = [](const json& object, const char* key) {
      const auto found = object.find(key);
      return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    const std::string source = text(b, "source");
    if (library::FindSource(source) == nullptr || !b.contains("titles") || !b["titles"].is_array()) {
      return SendError(res, 400, "invalid_body",
                       std::format("expected {}", kShape));
    }
    if (!s.config.GetBool("metadata.enabled")) return SendJson(res, {{"queued", 0}});

    std::vector<model::Game> titles;
    for (const json& entry : b["titles"]) {
      if (!entry.is_object()) continue;
      model::Game title;
      title.source = source;
      title.source_ref = text(entry, "ref");
      title.name = text(entry, "title");
      title.id = source + "-" + title.source_ref;
      if (!IsSafeRef(title.source_ref) || title.name.empty() || !metadata::TitleNeedsFetch(s.config, s.games.Metadata(), title.id)) continue;
      // How Fetch tells a Steam game apart.
      if (source == "steam") title.runner_ref = "steam:" + title.source_ref;
      titles.push_back(std::move(title));
    }
    SendJson(res, {{"queued", s.fetches.EnqueueTitles(s.config, s.events, std::move(titles))}}, 202);
  });

  // An installed title's game takes or drops the tag; the rest are kept as hidden titles.
  http.Post("/v1/library/hidden", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"source": "...", "refs": ["..."], "hidden": true})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const json& b = *body;
    const json refs = b.value("refs", json());
    const std::string source =
        b.contains("source") && b["source"].is_string() ? b["source"].get<std::string>() : "";
    if (library::FindSource(source) == nullptr || !refs.is_array() || !b.contains("hidden") ||
        !b["hidden"].is_boolean() || !std::ranges::all_of(refs, [](const json& ref) {
          return ref.is_string() && IsSafeRef(ref.get<std::string>());
        })) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const bool hide = b["hidden"].get<bool>();
    std::vector<std::string> games, titles;
    for (const json& ref : refs) {
      const std::string key = source + "-" + ref.get<std::string>();
      (s.games.Find(key) ? games : titles).push_back(key);
    }
    const auto changed = s.games.UpdateMany(games, [hide](model::Game& game) {
      const bool hidden = std::ranges::contains(game.tags, std::string("hidden"));
      if (hidden == hide) return false;
      if (hide) {
        game.tags.push_back("hidden");
      } else {
        std::erase(game.tags, std::string("hidden"));
      }
      return true;
    });
    if (!changed) return SendError(res, 500, changed.error());
    if (auto kept = s.games.SetTitlesHidden(titles, hide); !kept)
      return SendError(res, 500, kept.error());
    for (const model::Game& game : *changed) s.events.Publish("game.updated", s.Record(game));
    const json payload = {{"source", source}, {"refs", refs}, {"hidden", hide}};
    s.events.Publish("library.hidden_changed", payload);
    SendJson(res, payload);
  });

  http.Post("/v1/library/install", [library_install_or_update](const Request& req, Response& res) {
    library_install_or_update(req, res, false);
  });
  http.Post("/v1/library/update", [library_install_or_update](const Request& req, Response& res) {
    library_install_or_update(req, res, true);
  });
  http.Post("/v1/library/install/pause", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"source": "epic"|"gog"|"amazon", "ref": "..."})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const json& b = *body;
    if (!b.contains("source") || !b["source"].is_string() || !b.contains("ref") || !b["ref"].is_string()) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const std::string source = b["source"];
    const std::string ref = b["ref"];
    const library::ILibrarySource* src = library::FindSource(source);
    if (src == nullptr) {
      return SendError(res, 400, "unknown_source", std::format("no installable source named \"{}\"", source));
    }
    if (!src->CanPause()) {
      return SendError(res, 409, "pause_unsupported", std::format("{} installs can't be paused, only cancelled", source));
    }
    const std::string target = source + "-" + ref;
    const auto job = s.jobs.RunningFor(target);
    if (!job) return SendError(res, 409, "not_running", "no install of that title is running");
    const bool update = s.jobs.Find(*job).value_or(json::object()).value("kind", "") == "update";
    s.paused_installs.Add(target, {{"source", source}, {"ref", ref}, {"update", update}});
    if (const auto cancelled = s.jobs.Cancel(*job); !cancelled) {
      s.paused_installs.Remove(target);
      return SendError(res, 409, cancelled.error());
    }
    SendJson(res, json{{"status", "pausing"}, {"job", *job}});
  });
  http.Get("/v1/library/install/paused", [&s](const Request&, Response& res) {
    json paused = json::array();
    for (json& install : s.paused_installs.List()) {
      if (!s.Conceals(install.value("source", std::string()) + "-" +
                      install.value("ref", std::string())))
        paused.push_back(std::move(install));
    }
    SendJson(res, std::move(paused));
  });
  http.Delete("/v1/library/install/paused", [&s](const Request& req, Response& res) {
    const std::string source = Param(req, "source");
    const std::string ref = Param(req, "ref");
    if (!s.paused_installs.Remove(source + "-" + ref)) {
      return SendError(res, 404, "not_paused", "no paused install of that title");
    }
    s.events.Publish("library.install.failed",
                     FailedEvent({{"source", source}, {"ref", ref}, {"update", false}},
                                 Error{"cancelled", "Cancelled", "", {}}));
    SendJson(res, json{{"status", "cancelled"}});
  });

  // --- desktop entries --------------------------------------------------

  http.Get("/v1/desktop-entries/candidates", [&s](const Request&, Response& res) {
    desktop::DesktopEntryScanner scanner(s.config, s.games, s.events);
    auto candidates = scanner.ListCandidates();
    if (!candidates) return SendError(res, 404, candidates.error());
    json out = json::array();
    for (const auto& c : *candidates) out.push_back({{"id", c.id}, {"name", c.name}, {"icon", c.icon}});
    SendJson(res, std::move(out));
  });

  http.Post("/v1/desktop-entries/import", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"ids": ["..."]})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const auto listed = body->contains("ids") ? StringList(*body, "ids") : std::nullopt;
    if (!listed) return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    const std::vector<std::string>& ids = *listed;

    desktop::DesktopEntryScanner scanner(s.config, s.games, s.events);
    auto summary = scanner.Import(ids);
    if (!summary) return SendError(res, 404, summary.error());
    s.AfterImport(summary->added_games);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}});
  });

  http.Post("/v1/desktop-entries/sync", [&s](const Request&, Response& res) {
    s.SyncDesktopEntries();
    SendJson(res, {{"ok", true}});
  });
}

}  // namespace mira::api
