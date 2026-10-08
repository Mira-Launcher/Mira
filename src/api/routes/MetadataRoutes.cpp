#include "api/Routes.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "library/GamePatch.h"
#include "config/Resolver.h"
#include "metadata/MetadataFetcher.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

// A preview is saved without an extension, so its type comes from its bytes.
std::string SniffImageType(std::string_view bytes) {
  if (bytes.starts_with("\x89PNG")) return "image/png";
  if (bytes.starts_with("GIF8")) return "image/gif";
  if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP") return "image/webp";
  return "image/jpeg";
}

}  // namespace

// Serves one cached art slot for `id`, or 404s.
void SendCachedArtwork(const store::MetadataStore& cache, const std::string& id, const std::string& type,
                       Response& res) {
  const auto art = cache.ArtFor(id, type);
  if (!art) return SendError(res, 404, "artwork_not_found", "no artwork cached for this game yet");
  std::ifstream in(art->file, std::ios::binary);
  if (!in) return SendError(res, 404, "artwork_not_found", "cached artwork file is missing");
  std::ostringstream buffer;
  buffer << in.rdbuf();
  res.set_content(buffer.str(), art->content_type);
}

void RegisterMetadataRoutes(httplib::Server& http, Services& s) {
  // --- metadata ---------------------------------------------------------

  http.Get(R"(/v1/games/([^/]+)/metadata)", [&s](const Request& req, Response& res) {
    if (!s.games.Find(req.matches[1])) return SendError(res, 404, "game_not_found", "no such game");
    if (!s.games.Metadata().Has(req.matches[1])) {
      return SendError(res, 404, "metadata_not_found", "no metadata cached for this game yet");
    }
    SendJson(res, s.games.Metadata().Read(req.matches[1]));
  });

  http.Get(R"(/v1/games/([^/]+)/artwork)", [&s](const Request& req, Response& res) {
    if (!s.games.Find(req.matches[1])) return SendError(res, 404, "game_not_found", "no such game");
    SendCachedArtwork(s.games.Metadata(), req.matches[1], Param(req, "type", "cover"), res);
  });

  // Takes a candidate id, never a URL, so the daemon can't be made to fetch an
  // arbitrary address.
  http.Post(R"(/v1/games/([^/]+)/artwork)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!s.games.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    const auto body = BodyObject(req, res, R"({"candidate_id": <id>})");
    if (!body) return;
    const json& b = *body;
    if (!b.value("candidate_id", json()).is_number_integer()) {
      return SendError(res, 400, "invalid_json", "body must be {\"candidate_id\": <id>}");
    }
    const std::int64_t candidate_id = b["candidate_id"].get<std::int64_t>();
    s.StartJob(req, res, "artwork", id, "Choosing artwork",
               [&s, id, slot, candidate_id](JobRegistry::Progress&) -> Result<json> {
                 if (auto selected = metadata::SelectArtwork(s.config, s.games.Metadata(), id, slot, candidate_id); !selected) {
                   s.events.Publish("game.artwork_select_failed",
                                    FailedEvent({{"id", id}, {"type", slot}}, selected.error()));
                   return std::unexpected(selected.error());
                 }
                 s.events.Publish("game.artwork_selected", {{"id", id}, {"type", slot}});
                 return json{{"id", id}, {"type", slot}};
               },
               &s.artwork_selects);
  });

  http.Post(R"(/v1/games/([^/]+)/artwork/candidates)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!s.games.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    int page = 0;
    const std::string raw = Param(req, "page", "0");
    if (std::from_chars(raw.data(), raw.data() + raw.size(), page).ec != std::errc() || page < 0) {
      return SendError(res, 400, "invalid_page", "?page= must be 0 or more");
    }
    // Echoed back, so a caller can tell its answer from a replayed one.
    const std::string request = Param(req, "request");
    s.artwork_thumbs.Post([&s, id, slot, page, request] {
      json event = {{"id", id}, {"type", slot}, {"page", page}, {"request", request}};
      if (auto fetched = metadata::FetchCandidatePage(s.config, s.games.Metadata(), id, slot, page); fetched) {
        event.update(*fetched);
      } else {
        event = FailedEvent(std::move(event), fetched.error());
      }
      s.events.Publish("game.artwork_candidates_ready", event);
    });
    SendJson(res, {{"status", "fetching"}}, 202);
  });

  // Cached previews are ready at once; the rest are fetched in the background.
  http.Post(R"(/v1/games/([^/]+)/artwork/thumbs)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!s.games.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    const auto body = BodyObject(req, res, R"({"candidate_ids": [<id>, ...]})");
    if (!body) return;
    const json ids = body->value("candidate_ids", json());
    if (!ids.is_array() || ids.empty() || ids.size() > 64 ||
        !std::ranges::all_of(ids, [](const json& value) { return value.is_number_integer(); })) {
      return SendError(res, 400, "invalid_json", "body must be {\"candidate_ids\": [<id>, ...]}, 1 to 64 ids");
    }
    const std::vector<std::int64_t> candidate_ids = ids.get<std::vector<std::int64_t>>();
    s.artwork_thumbs.Post([&s, id, slot, candidate_ids] {
      json event = {{"id", id}, {"type", slot}};
      if (auto batch = metadata::FetchCandidateThumbs(s.config, s.games.Metadata(), id, slot, candidate_ids); batch) {
        event["ready"] = batch->ready;
        event["failed"] = batch->failed;
      } else {
        event["ready"] = json::array();
        event["failed"] = candidate_ids;
        event = FailedEvent(std::move(event), batch.error());
      }
      s.events.Publish("game.artwork_thumbs_ready", event);
    });
    SendJson(res, {{"status", "fetching"}}, 202);
  });

  http.Get(R"(/v1/games/([^/]+)/artwork/thumb)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!s.games.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    const std::string slot = Param(req, "type", "cover");
    std::int64_t candidate_id = 0;
    const std::string raw = Param(req, "candidate_id");
    if (std::from_chars(raw.data(), raw.data() + raw.size(), candidate_id).ec != std::errc() || raw.empty()) {
      return SendError(res, 400, "missing_candidate_id", "?candidate_id= is required");
    }
    const std::filesystem::path file = metadata::CandidateThumbFile(s.config, id, slot, candidate_id);
    std::ifstream in(file, std::ios::binary);
    if (file.empty() || !in) return SendError(res, 404, "thumb_not_cached", "no preview cached for that candidate");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    res.set_content(buffer.str(), SniffImageType(buffer.str()));
  });

  http.Delete("/v1/artwork/thumbs", [&s](const Request&, Response& res) {
    metadata::ClearCandidateThumbs(s.config);
    res.status = 204;
  });

  // force=true: an explicit refresh works even with metadata.enabled off.
  http.Post(R"(/v1/games/([^/]+)/metadata/refresh)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    // Only user-initiated refreshes report failure as a notification.
    const bool announce = BoolParam(req, "announce");
    s.fetches.Enqueue(s.config, s.events, *game, /*force=*/true, announce);
    SendJson(res, {{"status", "fetching"}}, 202);
  });

  http.Get(R"(/v1/games/([^/]+)/metadata/matches)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const std::string query = Param(req, "q", game->name);
    auto matches = metadata::SearchSteamGridDb(s.config, query);
    if (!matches) return SendError(res, 502, matches.error());
    const std::int64_t chosen = config::Resolver(s.config, game->overrides).GetInt("metadata.steamgriddb_id");
    SendJson(res, {{"query", query}, {"chosen", chosen}, {"matches", *matches}});
  });

  http.Post(R"(/v1/games/([^/]+)/metadata/wrong-match)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    auto matches = metadata::SearchSteamGridDb(s.config, game->name);
    if (!matches) return SendError(res, 502, matches.error());
    const std::int64_t current = config::Resolver(s.config, game->overrides).GetInt("metadata.steamgriddb_id");
    std::size_t next = 1;  // no choice yet: the top match was in use
    for (std::size_t i = 0; i < matches->size(); ++i) {
      if ((*matches)[i].value("id", std::int64_t{0}) == current) next = i + 1;
    }
    if (next >= matches->size()) {
      return SendError(res, 409, "no_more_matches",
                       "no other SteamGridDB match for this name -- pick one with ?q= on .../metadata/matches");
    }
    const json& match = (*matches)[next];
    const json patch = {{"metadata.steamgriddb_id", match.value("id", std::int64_t{0})}};
    auto updated = s.games.Update(game->id, [&](model::Game& g) { library::ApplyOverridesPatch(g, patch); });
    if (!updated) return SendError(res, 500, updated.error());
    s.fetches.Enqueue(s.config, s.events, *updated, /*force=*/true, /*announce=*/true);
    SendJson(res, {{"status", "fetching"}, {"match", match}}, 202);
  });

  http.Post(R"(/v1/games/([^/]+)/metadata/match)", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"steamgriddb_id": <id, or 0 for the top match>})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const json& b = *body;
    if (!b.contains("steamgriddb_id") || !b["steamgriddb_id"].is_number_integer() ||
        b["steamgriddb_id"].get<std::int64_t>() < 0) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const std::int64_t id = b["steamgriddb_id"];
    const json patch = {{"metadata.steamgriddb_id", id == 0 ? json(nullptr) : json(id)}};
    auto game = s.games.Update(req.matches[1], [&](model::Game& g) { library::ApplyOverridesPatch(g, patch); });
    if (!game) return SendError(res, 404, game.error());
    s.fetches.Enqueue(s.config, s.events, *game, /*force=*/true, /*announce=*/true);
    SendJson(res, {{"status", "fetching"}, {"steamgriddb_id", id}}, 202);
  });

  // POST /v1/games/{id}/metadata/refresh for many games in one request, unannounced, as a job.
  http.Post("/v1/games/metadata/refresh", [&s](const Request& req, Response& res) {
    const auto body = BodyObject(req, res, R"({"ids": [...]})");
    if (!body) return;
    const auto ids = StringList(*body, "ids");
    if (!ids) return SendError(res, 400, "invalid_body", R"(expected {"ids": [...]})");
    std::vector<model::Game> games;
    for (const std::string& id : *ids) {
      if (auto game = s.games.Find(id)) games.push_back(std::move(*game));
    }
    s.StartJob(req, res, "metadata", "", "Refreshing metadata",
             [&s, games = std::move(games)](JobRegistry::Progress& progress) { return s.RefreshMetadata(games, progress); });
  });

  http.Post("/v1/games/metadata/refresh-missing", [&s](const Request& req, Response& res) {
    std::vector<model::Game> games;
    for (const model::Game& game : s.games.All()) {
      if (!s.games.Metadata().ArtVersions(game.id).contains("cover")) games.push_back(game);
    }
    s.StartJob(req, res, "metadata", "", "Fetching missing cover art",
             [&s, games = std::move(games)](JobRegistry::Progress& progress) { return s.RefreshMetadata(games, progress); });
  });
}

}  // namespace mira::api
