#include "core/LogHub.h"
#include "api/Routes.h"

#include <algorithm>
#include <charconv>
#include <format>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "core/Log.h"
#include "humble/Humble.h"
#include "itch/Itch.h"
#include "library/Stores.h"
#include "runner/Downloader.h"

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

json StatusJson(const library::Store& store, const runner::AuthStatus& status) {
  return {{"id", store.id},
          {"name", store.name},
          {"tool", runner::ToJson(status.tool)},
          {"authenticated", status.authenticated},
          {"account", status.account}};
}

// The store a route's {id} names, or nullptr after a 404.
const library::Store* FindStoreOr404(const Request& req, Response& res) {
  const library::Store* store = library::FindStore(req.matches[1].str());
  if (store == nullptr) SendError(res, 404, "store_not_found", "no such store");
  return store;
}

}  // namespace

void RegisterStoreRoutes(httplib::Server& http, Services& s) {
  http.Get("/v1/stores", [](const Request&, Response& res) {
    json out = json::array();
    for (const library::Store& store : library::AllStores()) {
      out.push_back({{"id", store.id},
                     {"name", store.name},
                     {"tool_name", store.tool},
                     {"can_import", store.import != nullptr},
                     {"can_logout", store.logout != nullptr}});
    }
    SendJson(res, std::move(out));
  });

  http.Get(R"(/v1/stores/([a-z0-9-]+)/status)", [&s](const Request& req, Response& res) {
    const library::Store* store = FindStoreOr404(req, res);
    if (store) SendJson(res, StatusJson(*store, store->status(s.config)));
  });

  // Downloads the newest release of the store's command-line tool. Running it again is how it updates.
  http.Post(R"(/v1/stores/([a-z0-9-]+)/setup)", [&s](const Request& req, Response& res) {
    const library::Store* store = FindStoreOr404(req, res);
    if (!store) return;
    s.StartJob(req, res, "setup", store->id, std::format("Setting up {}", store->tool),
               [&s, store](JobRegistry::Progress&) -> Result<json> {
                 const std::string channel = std::string("setup:") + store->id;
                 loghub::Begin(channel);
                 const auto say = [&channel](const std::string& line) { loghub::Append(channel, line + "\n"); };
                 const auto fail = [&](const Error& error) -> Result<json> {
                   say("Failed: " + error.message);
                   loghub::End(channel);
                   return std::unexpected(error);
                 };
                 say(std::format("Looking up the newest {} release", store->tool));
                 const auto releases = runner::ListReleases(s.config, store->release_kind);
                 if (!releases) return fail(releases.error());
                 if (releases->empty()) {
                   return fail(Error{.code = "no_release_found",
                                     .message = std::format("no matching {} release found", store->tool),
                                     .hint = {}});
                 }
                 const runner::ReleaseAsset& asset = releases->front();  // newest first
                 say(std::format("Downloading {} {}", store->tool, asset.tag));
                 if (auto installed = store->install_tool(s.config, asset); !installed) return fail(installed.error());
                 log::Info("installed {} {}", store->tool, asset.tag);
                 say(std::format("Installed {} {}", store->tool, asset.tag));
                 loghub::End(channel);
                 return json{{"tag", asset.tag}};
               });
  });

  // The page to sign in at. Amazon makes a fresh one each time.
  http.Post(R"(/v1/stores/([a-z0-9-]+)/login/begin)", [&s](const Request& req, Response& res) {
    const library::Store* store = FindStoreOr404(req, res);
    if (!store) return;
    const auto url = store->begin_login(s.config);
    if (!url) return SendError(res, 409, url.error());
    SendJson(res, {{"url", *url}});
  });

  // Completes sign-in with what the user pasted: a code, an API key, a session key or a redirect URL.
  http.Post(R"(/v1/stores/([a-z0-9-]+)/login)", [&s](const Request& req, Response& res) {
    const library::Store* store = FindStoreOr404(req, res);
    if (!store) return;
    const auto body = BodyObject(req, res, R"({"credential": "..."})");
    if (!body) return;
    if (!body->contains("credential") || !(*body)["credential"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"credential": "..."})");
    }
    if (auto logged_in = store->login(s.config, (*body)["credential"].get<std::string>()); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    SendJson(res, StatusJson(*store, store->status(s.config)));
  });

  http.Post(R"(/v1/stores/([a-z0-9-]+)/logout)", [&s](const Request& req, Response& res) {
    const library::Store* store = FindStoreOr404(req, res);
    if (!store) return;
    if (store->logout == nullptr) {
      return SendError(res, 400, "logout_unsupported", std::format("{} keeps its own sign-in", store->tool));
    }
    SendResult(res, store->logout(s.config));
  });

  // Imports what the store's own tool reports as installed. gog only looks under gog.install_root,
  // since gogdl can't list installed games.
  http.Post(R"(/v1/stores/([a-z0-9-]+)/import)", [&s](const Request& req, Response& res) {
    const library::Store* store = FindStoreOr404(req, res);
    if (!store) return;
    if (store->import == nullptr) {
      return SendError(res, 400, "import_unsupported", std::format("{} has no installed games to import", store->name));
    }
    s.StartJob(req, res, "import", store->id, std::format("Importing from {}", store->name),
               [&s, store](JobRegistry::Progress&) -> Result<json> {
                 auto summary = store->import(s.config, s.games, s.events);
                 if (!summary) return std::unexpected(summary.error());
                 s.AfterImport(summary->added_games);
                 return json{{"added", summary->added}, {"updated", summary->updated}};
               });
  });

  // --- itch collections ---------------------------------------------------

  http.Get("/v1/stores/itch/collections", [&s](const Request&, Response& res) {
    auto collections = itch::ListCollections(s.config);
    if (!collections) return SendError(res, 400, collections.error());
    json out = json::array();
    for (const itch::ItchCollection& collection : *collections) out.push_back(CollectionJson(collection));
    SendJson(res, std::move(out));
  });

  http.Post("/v1/stores/itch/collections", [&s](const Request& req, Response& res) {
    const auto body = BodyObject(req, res, R"({"link": "https://itch.io/c/<id>/..."})");
    if (!body) return;
    if (!body->contains("link") || !(*body)["link"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"link": "https://itch.io/c/<id>/..."})");
    }
    auto added = itch::AddCollection(s.config, (*body)["link"].get<std::string>());
    if (!added) return SendError(res, 400, added.error());
    SendJson(res, CollectionJson(*added), 201);
  });

  http.Delete(R"(/v1/stores/itch/collections/(\d+))", [&s](const Request& req, Response& res) {
    std::int64_t id = 0;
    const std::string digits = req.matches[1].str();
    if (std::from_chars(digits.data(), digits.data() + digits.size(), id).ec != std::errc()) {
      return SendError(res, 400, "invalid_id", "that collection id is out of range");
    }
    SendResult(res, itch::RemoveCollection(s.config, id));
  });

  // --- humble bundles -----------------------------------------------------

  http.Get("/v1/stores/humble/bundles", [&s](const Request&, Response& res) {
    auto bundles = humble::ListBundles(s.config);
    if (!bundles) return SendError(res, 400, bundles.error());
    json out = json::array();
    for (const humble::BundleSummary& bundle : *bundles) {
      out.push_back({{"key", bundle.key}, {"name", bundle.name}, {"claimed", bundle.claimed}});
    }
    SendJson(res, std::move(out));
  });

  http.Post("/v1/stores/humble/download", [&s](const Request& req, Response& res) {
    const auto body = BodyObject(req, res, R"({"bundle_key": "...", "item_numbers": "..."})");
    if (!body) return;
    if (!body->contains("bundle_key") || !(*body)["bundle_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"bundle_key": "...", "item_numbers": "..."})");
    }
    const std::string bundle_key = (*body)["bundle_key"];
    const std::string item_numbers = body->contains("item_numbers") && (*body)["item_numbers"].is_string()
                                         ? (*body)["item_numbers"].get<std::string>()
                                         : std::string();
    // The key becomes a folder name and a humble-cli argument.
    if (bundle_key.empty() || !std::ranges::all_of(bundle_key, [](unsigned char c) { return std::isalnum(c); }) ||
        !std::ranges::all_of(item_numbers, [](unsigned char c) { return std::isdigit(c) || c == ',' || c == ' ' || c == '-'; })) {
      return SendError(res, 400, "invalid_body", "bundle_key must be letters and digits, and item_numbers digits, commas and dashes");
    }
    s.StartJob(req, res, "download", bundle_key, "Downloading a Humble bundle",
               [&s, bundle_key, item_numbers](JobRegistry::Progress&) -> Result<json> {
                 const Result<bool> downloaded = humble::Download(s.config, bundle_key, item_numbers);
                 if (!downloaded) return std::unexpected(downloaded.error());
                 if (!*downloaded) {
                   // A redeemed key with no Humble-hosted files, most likely.
                   return Err("nothing_to_download", "this bundle has nothing to download from Humble",
                              "It may be a key to redeem in another store.");
                 }
                 log::Info("humble download finished: {}", bundle_key);
                 return json{{"bundle_key", bundle_key}, {"path", humble::DownloadDir(s.config, bundle_key).string()}};
               });
  });
}

}  // namespace mira::api
