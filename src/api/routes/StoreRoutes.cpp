#include "api/Routes.h"

#include <algorithm>
#include <charconv>
#include <format>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "amazon/AmazonImporter.h"
#include "amazon/Nile.h"
#include "core/Log.h"
#include "epic/EpicImporter.h"
#include "epic/EpicInstaller.h"
#include "epic/Legendary.h"
#include "gog/Gog.h"
#include "gog/GogImporter.h"
#include "gog/GogInstaller.h"
#include "humble/Humble.h"
#include "itch/Itch.h"
#include "itch/ItchImporter.h"
#include "itch/ItchInstaller.h"
#include "launchers/Launchers.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

json CollectionJson(const itch::ItchCollection& collection) {
  return {{"id", collection.id},
          {"title", collection.title},
          {"games_count", collection.games_count},
          {"own", collection.own},
          {"url", std::format("https://itch.io/c/{}", collection.id)}};
}

}  // namespace

void RegisterStoreRoutes(httplib::Server& http, Services& s) {
  // A store's import as a job; `Importer` is its XxxImporter.
  const auto import_job = [&s]<typename Importer>(const Request& req, Response& res, std::type_identity<Importer>,
                                                    const std::string& source, const std::string& label) {
    s.StartJob(req, res, "import", source, label, [&s](JobRegistry::Progress&) -> Result<json> {
      Importer importer(s.config, s.games, s.events);
      auto summary = importer.Import();
      if (!summary) return std::unexpected(summary.error());
      s.AfterImport(summary->added_games);
      return json{{"added", summary->added}, {"updated", summary->updated}};
    });
  };

  // --- store CLI setup ----------------------------------------------------

  // POST <route> downloads the newest release of a store's CLI in the
  // background. Re-running it fetches the latest release, which is also how
  // updates work. Progress arrives as <event_prefix>.started/.finished/.failed.
  using BinaryInstaller = Result<void> (*)(const config::Config&, const runner::ReleaseAsset&);
  const auto register_cli_setup = [&s, &http](const char* route, const std::string& kind, const std::string& tool,
                                         const std::string& event_prefix, BinaryInstaller install) {
    http.Post(route, [&s, kind, tool, event_prefix, install](const Request&, Response& res) {
      auto releases = runner::ListReleases(s.config, kind);
      if (!releases) return SendError(res, 502, releases.error());
      if (releases->empty()) {
        return SendError(res, 404, "no_release_found", std::format("no matching {} release found", tool));
      }

      const runner::ReleaseAsset asset = releases->front();  // newest first
      s.events.Publish(event_prefix + ".started", {{"tag", asset.tag}});
      s.operations.Post([&s, asset, tool, event_prefix, install] {
        if (auto installed = install(s.config, asset); !installed) {
          log::Error("{} install failed ({}): {}", tool, asset.tag, installed.error().message);
          s.events.Publish(event_prefix + ".failed", FailedEvent({{"tag", asset.tag}}, installed.error()));
        } else {
          log::Info("installed {} {}", tool, asset.tag);
          s.events.Publish(event_prefix + ".finished", {{"tag", asset.tag}});
        }
      });

      SendJson(res, {{"status", "downloading"}, {"tag", asset.tag}}, 202);
    });
  };
  register_cli_setup("/v1/epic/legendary/install", "legendary", "legendary", "epic.legendary.install",
                     epic::InstallLegendaryBinary);
  register_cli_setup("/v1/gog/setup", "gog", "gogdl", "gog.setup", gog::InstallGogBinary);
  register_cli_setup("/v1/amazon/setup", "amazon", "nile", "amazon.setup", amazon::InstallNileBinary);
  register_cli_setup("/v1/itch/setup", "itch", "butler", "itch.setup", itch::InstallButlerBinary);
  register_cli_setup("/v1/humble/setup", "humble", "humble-cli", "humble.setup", humble::InstallHumbleCliBinary);

  // --- epic -------------------------------------------------------------

  http.Get("/v1/epic/legendary/status", [&s](const Request&, Response& res) {
    const runner::ToolStatus status = epic::DetectLegendary(s.config);
    SendJson(res, {{"installed", status.installed},
                  {"source", status.source},
                  {"path", status.path},
                  {"version", status.version}});
  });

  http.Get("/v1/epic/status", [&s](const Request&, Response& res) {
    const epic::EpicAuthStatus status = epic::Status(s.config);
    SendJson(res, {{"legendary", {{"installed", status.legendary.installed},
                                  {"source", status.legendary.source},
                                  {"path", status.legendary.path},
                                  {"version", status.legendary.version}}},
                  {"authenticated", status.authenticated},
                  {"account", status.account},
                  {"login_url", epic::kLoginUrl}});
  });

  http.Post("/v1/epic/auth", [&s](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("code") || !body["code"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"code": "..."})");
    }
    if (auto logged_in = epic::Login(s.config, body["code"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    const epic::EpicAuthStatus status = epic::Status(s.config);
    SendJson(res, {{"authenticated", status.authenticated}, {"account", status.account}});
  });

  http.Post("/v1/epic/logout", [&s](const Request&, Response& res) {
    if (auto logged_out = epic::Logout(s.config); !logged_out) {
      return SendError(res, 400, logged_out.error());
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  // --- gog ----------------------------------------------------------------

  http.Get("/v1/gog/status", [&s](const Request&, Response& res) {
    const gog::GogAuthStatus status = gog::Status(s.config);
    SendJson(res, {{"gogdl", {{"installed", status.gogdl.installed},
                              {"source", status.gogdl.source},
                              {"path", status.gogdl.path},
                              {"version", status.gogdl.version}}},
                  {"authenticated", status.authenticated},
                  {"login_url", gog::kLoginUrl}});
  });

  http.Post("/v1/gog/auth", [&s](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("code") || !body["code"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"code": "..."})");
    }
    if (auto logged_in = gog::Login(s.config, body["code"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    const gog::GogAuthStatus status = gog::Status(s.config);
    SendJson(res, {{"authenticated", status.authenticated}});
  });

  http.Post("/v1/gog/logout", [&s](const Request&, Response& res) {
    if (auto logged_out = gog::Logout(s.config); !logged_out) {
      return SendError(res, 400, logged_out.error());
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  // --- amazon -------------------------------------------------------------

  http.Get("/v1/amazon/status", [&s](const Request&, Response& res) {
    const amazon::AmazonAuthStatus status = amazon::Status(s.config);
    SendJson(res, {{"nile", {{"installed", status.nile.installed},
                             {"source", status.nile.source},
                             {"path", status.nile.path},
                             {"version", status.nile.version}}},
                  {"authenticated", status.authenticated}});
  });

  http.Post("/v1/amazon/login", [&s](const Request&, Response& res) {
    const auto url = amazon::BeginLogin(s.config);
    if (!url) return SendError(res, 409, url.error());
    SendJson(res, {{"url", *url}});
  });

  http.Post("/v1/amazon/auth", [&s](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("redirect") || !body["redirect"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"redirect": "..."})");
    }
    if (auto logged_in = amazon::FinishLogin(s.config, body["redirect"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    SendJson(res, {{"authenticated", true}});
  });

  http.Post("/v1/amazon/logout", [&s](const Request&, Response& res) {
    if (auto logged_out = amazon::Logout(s.config); !logged_out) {
      return SendError(res, 400, logged_out.error());
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  // POST <route> imports what the store's own tool reports as installed, as a job.
  // gog only looks under gog.install_root, since gogdl can't list installed games.
  http.Post("/v1/epic/import", [import_job](const Request& req, Response& res) {
    import_job(req, res, std::type_identity<epic::EpicImporter>(), "epic", "Importing from Epic Games");
  });
  http.Post("/v1/gog/import", [import_job](const Request& req, Response& res) {
    import_job(req, res, std::type_identity<gog::GogImporter>(), "gog", "Importing from GOG");
  });
  http.Post("/v1/amazon/import", [import_job](const Request& req, Response& res) {
    import_job(req, res, std::type_identity<amazon::AmazonImporter>(), "amazon", "Importing from Amazon Games");
  });
  http.Post("/v1/itch/import", [import_job](const Request& req, Response& res) {
    import_job(req, res, std::type_identity<itch::ItchImporter>(), "itch", "Importing from itch.io");
  });

  // --- store launchers --------------------------------------------------

  http.Get("/v1/launchers", [&s](const Request&, Response& res) {
    json list = json::array();
    for (const launchers::Launcher& launcher : launchers::All()) {
      const auto game = s.games.Find(launchers::GameId(launcher));
      list.push_back({{"id", launcher.id},
                      {"name", launcher.name},
                      {"game_id", launchers::GameId(launcher)},
                      {"installed", launchers::Installed(s.games, launcher)},
                      {"install_state", launchers::InstallState(launcher)},
                      {"interactive_install", launcher.interactive},
                      {"prefix", game ? game->data_dir : ""},
                      {"runner_ref", game ? game->runner_ref : ""},
                      {"error", game ? game->last_error : ""}});
    }
    SendJson(res, list);
  });

  http.Post(R"(/v1/launchers/([^/]+)/install)", [&s](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    if (!launchers::BeginInstall(*launcher)) {
      return SendError(res, 409, "install_running", std::format("{} is already installing", launcher->name));
    }
    s.events.Publish("launcher.install.started", {{"id", launcher->id}});
    s.operations.Post([&s, launcher] {
      const auto done = launchers::Install(s.config, s.games, *launcher);
      if (const auto stored = s.games.Find(launchers::GameId(*launcher))) {
        s.events.Publish("game.updated", s.Record(*stored));
      }
      if (!done) {
        s.events.Publish("launcher.install.failed", FailedEvent({{"id", launcher->id}}, done.error()));
        return;
      }
      if (const auto imported = launchers::Import(s.config, s.games, s.events, *launcher)) {
        for (const model::Game& game : imported->added_games) s.fetches.Enqueue(s.config, s.events, game);
      }
      s.SyncDesktopEntries();
      s.events.Publish("launcher.install.finished", {{"id", launcher->id}});
    });
    SendJson(res, {{"status", "installing"}, {"id", launcher->id}}, 202);
  });

  http.Post(R"(/v1/launchers/([^/]+)/import)", [&s](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    s.StartJob(req, res, "import", launcher->id, "Importing from " + launcher->name,
             [&s, launcher](JobRegistry::Progress&) -> Result<json> {
               const auto summary = launchers::Import(s.config, s.games, s.events, *launcher);
               if (!summary) return std::unexpected(summary.error());
               s.AfterImport(summary->added_games);
               return json{{"added", summary->added}, {"updated", summary->updated}};
             });
  });

  http.Post(R"(/v1/launchers/([^/]+)/open)", [&s](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return SendError(res, 400, "invalid_body", "expected a JSON object");
    const std::string action = body.value("action", std::string("launch"));
    if (action != "launch" && action != "install") {
      return SendError(res, 400, "invalid_action", "action must be launch or install");
    }
    model::Game target;
    target.source = "launcher";
    target.source_ref = launcher->id;
    if (const std::string ref = body.value("ref", std::string()); !ref.empty()) {
      target.source = launcher->id;
      target.source_ref = ref;
    }
    auto command = launchers::BuildCommand(s.config, s.games, target, action);
    if (!command) return SendError(res, 409, command.error());
    if (auto spawned = runner::SpawnDetached(*command); !spawned) {
      return SendError(res, 500, spawned.error());
    }
    SendJson(res, {{"status", "opened"}});
  });

  // --- itch -----------------------------------------------------------

  http.Get("/v1/itch/status", [&s](const Request&, Response& res) {
    const itch::ItchAuthStatus status = itch::Status(s.config);
    SendJson(res, {{"butler", {{"installed", status.butler.installed},
                               {"source", status.butler.source},
                               {"path", status.butler.path},
                               {"version", status.butler.version}}},
                  {"authenticated", status.authenticated},
                  {"login_url", itch::kApiKeysUrl}});
  });

  http.Post("/v1/itch/auth", [&s](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("api_key") || !body["api_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"api_key": "..."})");
    }
    if (auto logged_in = itch::Login(s.config, body["api_key"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    SendJson(res, {{"authenticated", true}});
  });

  http.Post("/v1/itch/logout", [&s](const Request&, Response& res) {
    if (auto logged_out = itch::Logout(s.config); !logged_out) {
      return SendError(res, 400, logged_out.error());
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  http.Get("/v1/itch/collections", [&s](const Request&, Response& res) {
    auto collections = itch::ListCollections(s.config);
    if (!collections) return SendError(res, 400, collections.error());
    json out = json::array();
    for (const itch::ItchCollection& collection : *collections) out.push_back(CollectionJson(collection));
    SendJson(res, std::move(out));
  });

  http.Post("/v1/itch/collections", [&s](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (!body.is_object() || !body.contains("link") || !body["link"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"link": "https://itch.io/c/<id>/..."})");
    }
    auto added = itch::AddCollection(s.config, body["link"].get<std::string>());
    if (!added) return SendError(res, 400, added.error());
    SendJson(res, CollectionJson(*added), 201);
  });

  http.Delete(R"(/v1/itch/collections/(\d+))", [&s](const Request& req, Response& res) {
    std::int64_t id = 0;
    const std::string digits = req.matches[1].str();
    if (std::from_chars(digits.data(), digits.data() + digits.size(), id).ec != std::errc()) {
      return SendError(res, 400, "invalid_id", "that collection id is out of range");
    }
    SendResult(res, itch::RemoveCollection(s.config, id));
  });

  // --- humble -------------------------------------------------------------

  http.Get("/v1/humble/status", [&s](const Request&, Response& res) {
    const humble::HumbleAuthStatus status = humble::Status(s.config);
    SendJson(res, {{"humble_cli", {{"installed", status.humble_cli.installed},
                                   {"source", status.humble_cli.source},
                                   {"path", status.humble_cli.path},
                                   {"version", status.humble_cli.version}}},
                  {"authenticated", status.authenticated},
                  {"login_url", humble::kLoginUrl}});
  });

  http.Post("/v1/humble/auth", [&s](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("session_key") || !body["session_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"session_key": "..."})");
    }
    if (auto logged_in = humble::Login(s.config, body["session_key"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    SendJson(res, {{"authenticated", true}});
  });

  http.Get("/v1/humble/library", [&s](const Request&, Response& res) {
    auto bundles = humble::ListBundles(s.config);
    if (!bundles) return SendError(res, 400, bundles.error());
    json out = json::array();
    for (const humble::BundleSummary& bundle : *bundles) {
      out.push_back({{"key", bundle.key}, {"name", bundle.name}, {"claimed", bundle.claimed}});
    }
    SendJson(res, std::move(out));
  });

  http.Post("/v1/humble/download", [&s](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("bundle_key") || !body["bundle_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"bundle_key": "...", "item_numbers": "..."})");
    }
    const std::string bundle_key = body["bundle_key"];
    const std::string item_numbers = body.contains("item_numbers") && body["item_numbers"].is_string()
                                         ? body["item_numbers"].get<std::string>()
                                         : std::string();
    // The key becomes a folder name and a humble-cli argument.
    if (bundle_key.empty() || !std::ranges::all_of(bundle_key, [](unsigned char c) { return std::isalnum(c); }) ||
        !std::ranges::all_of(item_numbers, [](unsigned char c) { return std::isdigit(c) || c == ',' || c == ' ' || c == '-'; })) {
      return SendError(res, 400, "invalid_body", "bundle_key must be letters and digits, and item_numbers digits, commas and dashes");
    }

    s.events.Publish("humble.download.started", {{"bundle_key", bundle_key}});
    s.operations.Post([&s, bundle_key, item_numbers] {
      const Result<bool> result = humble::Download(s.config, bundle_key, item_numbers);
      if (!result) {
        log::Error("humble download failed ({}): {}", bundle_key, result.error().message);
        s.events.Publish("humble.download.failed", FailedEvent({{"bundle_key", bundle_key}}, result.error()));
      } else if (!*result) {
        log::Warn("humble download for {} had nothing to download (a redeemed key with no Humble-hosted "
                 "files, most likely)",
                 bundle_key);
        s.events.Publish("humble.download.finished",
                       {{"bundle_key", bundle_key},
                        {"path", humble::DownloadDir(s.config, bundle_key).string()},
                        {"downloaded", false}});
      } else {
        log::Info("humble download finished: {}", bundle_key);
        s.events.Publish("humble.download.finished",
                       {{"bundle_key", bundle_key},
                        {"path", humble::DownloadDir(s.config, bundle_key).string()},
                        {"downloaded", true}});
      }
    });

    SendJson(res, {{"status", "downloading"}, {"bundle_key", bundle_key},
                  {"path", humble::DownloadDir(s.config, bundle_key).string()}},
            202);
  });
}

}  // namespace mira::api
