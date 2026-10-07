#include <httplib.h>

#include <algorithm>
#include <format>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "core/Log.h"
#include "desktop/DesktopEntryScanner.h"
#include "library/Catalog.h"
#include "library/Relocate.h"
#include "library/Scanner.h"
#include "library/SourceRegistry.h"
#include "library/SourceRemoval.h"
#include "library/SourceRunner.h"
#include "lutris/LutrisImporter.h"
#include "steam/FriendsStatus.h"
#include "steam/SteamScanner.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

}  // namespace

void RegisterLibraryRoutes(httplib::Server& http, Services& s) {
  // --- library ------------------------------------------------------------

  http.Post("/v1/library/scan", [&s](const Request& req, Response& res) {
    s.StartJob(req, res, "scan", "", "Scanning your library", [&s](JobRegistry::Progress&) -> Result<json> {
      library::Scanner scanner(s.config, s.games, s.events);
      scanner.UseMetadataQueue(s.fetches);
      scanner.UseInstallLane(s.installs);
      const library::ScanSummary summary = scanner.ScanAll();
      for (const model::Game& game : summary.added_games) s.fetches.Enqueue(s.config, s.events, game);
      return json{{"added", summary.added}, {"missing", summary.missing}, {"restored", summary.restored}};
    });
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
                 s.events.Publish("game.updated", s.Record(*saved));
                 ++moved;
               }
               s.SyncDesktopEntries();
               const int failed = static_cast<int>(errors.size());
               return json{{"moved", moved}, {"failed", failed}, {"errors", std::move(errors)}};
             });
  });

  // --- steam ------------------------------------------------------------

  http.Post("/v1/steam/scan", [&s](const Request& req, Response& res) {
    s.StartJob(req, res, "import", "steam", "Importing from Steam", [&s](JobRegistry::Progress&) -> Result<json> {
      steam::SteamScanner scanner(s.config, s.games, s.events);
      auto summary = scanner.Scan();
      if (!summary) return std::unexpected(summary.error());
      s.AfterImport(summary->added_games);
      return json{{"added", summary->added}, {"updated", summary->updated}};
    });
  });

  http.Post("/v1/steam/status", [](const Request& req, Response& res) {
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object())
      return SendError(res, 400, "invalid_body", "expected a JSON object");
    const auto set = steam::SetFriendsStatus(body.value("status", std::string()));
    if (!set) return SendError(res, set.error().code == "invalid_status" ? 400 : 409, set.error());
    SendJson(res, {{"status", body.value("status", std::string())}});
  });

  // --- lutris -----------------------------------------------------------

  http.Post("/v1/lutris/import", [&s](const Request& req, Response& res) {
    s.StartJob(req, res, "import", "lutris", "Importing from Lutris", [&s](JobRegistry::Progress&) -> Result<json> {
      lutris::LutrisImporter importer(s.config, s.games, s.events);
      auto summary = importer.Import();
      if (!summary) return std::unexpected(summary.error());
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
                   {"launcher_dir", plan->launcher_dir},
                   {"kept", plan->kept},
                   {"signs_out", plan->signs_out}});
  });

  http.Post(R"(/v1/sources/([a-z0-9-]+)/remove)", [&s](const Request& req, Response& res) {
    const std::string source = req.matches[1].str();
    // Checked now, so an unknown source is a 404 rather than a failed job.
    if (auto plan = library::PlanRemoval(s.config, s.games, source); !plan) return SendError(res, 404, plan.error());
    s.StartJob(req, res, "remove_source", source, "Removing " + source,
             [&s, source](JobRegistry::Progress&) -> Result<json> {
               auto removed = library::RemoveSource(s.config, s.games, s.events, source);
               if (!removed) return std::unexpected(removed.error());
               s.SyncDesktopEntries();
               return json{{"removed", removed->removed}, {"problems", removed->problems}};
             });
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
    const json body = json::parse(req.body, nullptr, false);
    if (!body.is_object() || !body.contains("runner_ref") || !body["runner_ref"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"runner_ref": "kind:name", "apply_to_games"?: bool})");
    }
    const auto runner = library::SetSourceRunner(s.config, s.games, req.matches[1].str(),
                                                 body["runner_ref"].get<std::string>(),
                                                 body.value("apply_to_games", false));
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
    auto entries = library::ListCatalog(s.config, s.games, source);
    if (!entries) return SendError(res, 400, entries.error());
    json out = json::array();
    for (const library::CatalogEntry& entry : *entries) {
      out.push_back({{"source", entry.source},
                     {"ref", entry.ref},
                     {"title", entry.title},
                     {"installed", entry.installed},
                     {"game_id", entry.game_id},
                     {"play_seconds", entry.play_seconds},
                     {"owned", entry.owned}});
    }
    SendJson(res, std::move(out));
  });

  // Keyed by {source, ref}, since the title isn't tracked yet. "update" in the
  // event payload tells an update from an install.
  auto library_install_or_update = [&s](const Request& req, Response& res, bool is_update) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("source") || !body["source"].is_string() ||
        !body.contains("ref") || !body["ref"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"source": "epic"|"steam"|"gog"|"itch"|"amazon", "ref": "..."})");
    }
    const std::string source = body["source"];
    const std::string ref = body["ref"];

    library::ILibrarySource* src = library::FindSource(source);
    if (src == nullptr) {
      return SendError(res, 400, "unknown_source", std::format("no installable source named \"{}\"", source));
    }
    if (!IsSafeRef(ref)) return SendError(res, 400, "invalid_ref", "that ref isn't a store id");

    s.events.Publish("library.install.started", {{"source", source}, {"ref", ref}, {"update", is_update}});
    s.StartJob(req, res, is_update ? "update" : "install", source + "-" + ref, (is_update ? "Updating " : "Installing ") + ref,
               [&s, src, source, ref, is_update](JobRegistry::Progress&) -> Result<json> {
                 const Result<void> result = is_update ? src->Update(s.config, s.games, s.events, ref)
                                                       : src->Install(s.config, s.games, s.events, ref);
                 if (!result) {
                   log::Error("{} {} failed ({}): {}", source, is_update ? "update" : "install", ref,
                              result.error().message);
                   s.events.Publish("library.install.failed",
                                    FailedEvent({{"source", source}, {"ref", ref}, {"update", is_update}}, result.error()));
                   return std::unexpected(result.error());
                 }
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
    SendCachedArtwork(s.config, source + "-" + ref, "cover", res);
  });

  http.Post("/v1/library/artwork", [&s](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    const auto text = [](const json& object, const char* key) {
      const auto found = object.find(key);
      return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    const std::string source = body.is_object() ? text(body, "source") : "";
    if (library::FindSource(source) == nullptr || !body.contains("titles") || !body["titles"].is_array()) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"source": "...", "titles": [{"ref": "...", "title": "..."}]})");
    }
    if (!s.config.GetBool("metadata.enabled")) return SendJson(res, {{"queued", 0}});

    std::vector<model::Game> titles;
    for (const json& entry : body["titles"]) {
      if (!entry.is_object()) continue;
      model::Game title;
      title.source = source;
      title.source_ref = text(entry, "ref");
      title.name = text(entry, "title");
      title.id = source + "-" + title.source_ref;
      if (!IsSafeRef(title.source_ref) || title.name.empty() || s.art_index.For(title.id).contains("cover")) continue;
      // How Fetch tells a Steam game apart.
      if (source == "steam") title.runner_ref = "steam:" + title.source_ref;
      titles.push_back(std::move(title));
    }
    SendJson(res, {{"queued", s.fetches.EnqueueTitles(s.config, s.events, std::move(titles))}}, 202);
  });

  http.Post("/v1/library/install", [library_install_or_update](const Request& req, Response& res) {
    library_install_or_update(req, res, false);
  });
  http.Post("/v1/library/update", [library_install_or_update](const Request& req, Response& res) {
    library_install_or_update(req, res, true);
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
    const json body = json::parse(req.body, nullptr, false);
    const auto listed = body.is_object() && body.contains("ids") ? StringList(body, "ids") : std::nullopt;
    if (!listed) return SendError(res, 400, "invalid_body", R"(expected {"ids": ["..."]})");
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
