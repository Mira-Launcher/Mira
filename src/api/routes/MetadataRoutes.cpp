#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "config/Resolver.h"
#include "library/GamePatch.h"
#include "library/SourceRegistry.h"
#include "metadata/ImageFit.h"
#include "metadata/MetadataFetcher.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

constexpr std::size_t kMaxUpload = 32 << 20;

// The game `id` names, or else a store title that isn't installed, by the id it gets once
// installed ("<source>-<ref>"), from that source's stored list. A title's art is chosen the same
// way as a game's.
std::optional<model::Game> ArtSubject(Services& s, const std::string& id) {
  if (auto game = s.games.Find(id)) return game;
  const std::size_t dash = id.find('-');
  if (dash == std::string::npos) return std::nullopt;
  const std::string source = id.substr(0, dash);
  if (library::FindSource(source) == nullptr) return std::nullopt;
  const auto rows = s.games.Metadata().ReadCatalog(source);
  if (!rows) return std::nullopt;
  const auto row =
      std::ranges::find(*rows, id.substr(dash + 1), &store::MetadataStore::CatalogRow::ref);
  if (row == rows->end()) return std::nullopt;
  model::Game title;
  title.id = id;
  title.name = row->title;
  title.source = source;
  title.source_ref = row->ref;
  if (source == "steam") title.runner_ref = "steam:" + row->ref;
  return title;
}

bool HasArtSubject(Services& s, const std::string& id) {
  return s.games.Contains(id) || ArtSubject(s, id).has_value();
}

}  // namespace

// Serves one cached art slot for `id`, or 404s.
void SendCachedArtwork(const store::MetadataStore& cache, const std::string& id,
                       const std::string& type, Response& res) {
  const auto art = cache.ArtFor(id, type);
  if (!art) return SendError(res, 404, "artwork_not_found", "no artwork cached for this game yet");
  std::ifstream in(art->file, std::ios::binary | std::ios::ate);
  if (!in) return SendError(res, 404, "artwork_not_found", "cached artwork file is missing");
  // Read straight into the reply: no stream copy per cover.
  std::string bytes(static_cast<std::size_t>(in.tellg()), '\0');
  in.seekg(0);
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  // From the bytes, not the record: older records call an .ico a JPEG.
  const std::string mime = metadata::ImageContentType(bytes);
  res.set_content(std::move(bytes), mime);
}

void RegisterMetadataRoutes(Router& http, Services& s) {
  // --- metadata ---------------------------------------------------------

  http.Get(R"(/v1/games/([^/]+)/metadata)", [&s](const Request& req, Response& res) {
    if (!HasArtSubject(s, req.matches[1]))
      return SendError(res, 404, "game_not_found", "no such game");
    if (!s.games.Metadata().Has(req.matches[1])) {
      return SendError(res, 404, "metadata_not_found", "no metadata cached for this game yet");
    }
    SendJson(res, s.games.Metadata().Read(req.matches[1]));
  });

  http.Get(R"(/v1/games/([^/]+)/artwork)", [&s](const Request& req, Response& res) {
    if (!HasArtSubject(s, req.matches[1]))
      return SendError(res, 404, "game_not_found", "no such game");
    SendCachedArtwork(s.games.Metadata(), req.matches[1], Param(req, "type", "cover"), res);
  });

  // Takes a candidate id, never a URL, so the daemon can't be made to fetch an
  // arbitrary address.
  http.Post(R"(/v1/games/([^/]+)/artwork)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!HasArtSubject(s, id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    const auto body = BodyObject(req, res, R"({"candidate_id": <id>})");
    if (!body) return;
    const json& b = *body;
    if (!b.value("candidate_id", json()).is_number_integer()) {
      return SendError(res, 400, "invalid_json", "body must be {\"candidate_id\": <id>}");
    }
    const std::int64_t candidate_id = b["candidate_id"].get<std::int64_t>();
    s.StartJob(
        req, res, "artwork", id, "Choosing artwork",
        [&s, id, slot, candidate_id](JobRegistry::Progress&) -> Result<json> {
          if (auto selected =
                  metadata::SelectArtwork(s.config, s.games.Metadata(), id, slot, candidate_id);
              !selected) {
            s.events.Publish("game.artwork_select_failed",
                             FailedEvent({{"id", id}, {"type", slot}}, selected.error()));
            return std::unexpected(selected.error());
          }
          s.SyncSteamArt(id);
          s.events.Publish("game.artwork_selected", {{"id", id}, {"type", slot}});
          return json{{"id", id}, {"type", slot}};
        },
        &s.artwork_selects);
  });

  // The body is the image itself, PNG or JPEG.
  http.Put(R"(/v1/games/([^/]+)/artwork)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!HasArtSubject(s, id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    if (req.body.size() > kMaxUpload)
      return SendError(res, 413, "image_too_large", "the image is over 32 MB");
    const std::string slot = req.get_param_value("type");
    s.StartJob(
        req, res, "artwork", id, "Using your image",
        [&s, id, slot, bytes = req.body](JobRegistry::Progress&) -> Result<json> {
          if (auto uploaded =
                  metadata::UploadArtwork(s.config, s.games.Metadata(), id, slot, bytes);
              !uploaded) {
            s.events.Publish("game.artwork_select_failed",
                             FailedEvent({{"id", id}, {"type", slot}}, uploaded.error()));
            return std::unexpected(uploaded.error());
          }
          s.SyncSteamArt(id);
          s.events.Publish("game.artwork_selected", {{"id", id}, {"type", slot}});
          return json{{"id", id}, {"type", slot}};
        },
        &s.artwork_selects);
  });

  http.Delete(R"(/v1/games/([^/]+)/artwork)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!HasArtSubject(s, id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    s.StartJob(
        req, res, "artwork", id, "Removing artwork",
        [&s, id, slot](JobRegistry::Progress&) -> Result<json> {
          if (auto removed = metadata::RemoveArtwork(s.config, s.games.Metadata(), id, slot);
              !removed) {
            s.events.Publish("game.artwork_select_failed",
                             FailedEvent({{"id", id}, {"type", slot}}, removed.error()));
            return std::unexpected(removed.error());
          }
          s.SyncDesktopEntry(id);  // its icon
          s.SyncSteamArt(id);
          s.events.Publish("game.artwork_selected", {{"id", id}, {"type", slot}});
          return json{{"id", id}, {"type", slot}};
        },
        &s.artwork_selects);
  });

  http.Post(R"(/v1/games/([^/]+)/artwork/candidates)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    const auto subject = ArtSubject(s, id);
    if (!subject) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    int page = 0;
    const std::string raw = Param(req, "page", "0");
    if (std::from_chars(raw.data(), raw.data() + raw.size(), page).ec != std::errc() || page < 0) {
      return SendError(res, 400, "invalid_page", "?page= must be 0 or more");
    }
    // Echoed back, so a caller can tell its answer from a replayed one.
    const std::string request = Param(req, "request");
    const metadata::CandidateFilter filter = {.style = Param(req, "style"),
                                              .adult = Param(req, "adult") == "1"};
    s.artwork_thumbs.Post([&s, id, slot, page, request, filter, name = subject->name] {
      json event = {{"id", id}, {"type", slot}, {"page", page}, {"request", request}};
      if (auto fetched = metadata::FetchCandidatePage(s.config, s.games.Metadata(), id, slot, page,
                                                      name, filter);
          fetched) {
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
    if (!HasArtSubject(s, id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    const auto body = BodyObject(req, res, R"({"candidate_ids": [<id>, ...]})");
    if (!body) return;
    const json ids = body->value("candidate_ids", json());
    if (!ids.is_array() || ids.empty() || ids.size() > 64 ||
        !std::ranges::all_of(ids, [](const json& value) { return value.is_number_integer(); })) {
      return SendError(res, 400, "invalid_json",
                       "body must be {\"candidate_ids\": [<id>, ...]}, 1 to 64 ids");
    }
    const std::vector<std::int64_t> candidate_ids = ids.get<std::vector<std::int64_t>>();
    s.artwork_thumbs.Post([&s, id, slot, candidate_ids] {
      json event = {{"id", id}, {"type", slot}};
      if (auto batch =
              metadata::FetchCandidateThumbs(s.config, s.games.Metadata(), id, slot, candidate_ids);
          batch) {
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
    if (!HasArtSubject(s, id)) return SendError(res, 404, "game_not_found", "no such game");
    const std::string slot = Param(req, "type", "cover");
    std::int64_t candidate_id = 0;
    const std::string raw = Param(req, "candidate_id");
    if (std::from_chars(raw.data(), raw.data() + raw.size(), candidate_id).ec != std::errc() ||
        raw.empty()) {
      return SendError(res, 400, "missing_candidate_id", "?candidate_id= is required");
    }
    const std::filesystem::path file =
        metadata::CandidateThumbFile(s.config, id, slot, candidate_id);
    std::ifstream in(file, std::ios::binary);
    if (file.empty() || !in)
      return SendError(res, 404, "thumb_not_cached", "no preview cached for that candidate");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    res.set_content(buffer.str(), metadata::ImageContentType(buffer.str()));
  });

  http.Delete("/v1/artwork/thumbs", [&s](const Request&, Response& res) {
    metadata::ClearCandidateThumbs(s.config);
    res.status = 204;
  });

  // As a game's settings close: the previews fetched while they were open.
  http.Delete(R"(/v1/games/([^/]+)/artwork/thumbs)", [&s](const Request& req, Response& res) {
    if (!HasArtSubject(s, req.matches[1]))
      return SendError(res, 404, "game_not_found", "no such game");
    metadata::ClearCandidateThumbs(s.config, req.matches[1]);
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
    auto game = ArtSubject(s, req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const std::string query = Param(req, "q", game->name);
    auto matches = metadata::SearchSteamGridDb(s.config, query);
    if (!matches) return SendError(res, 502, matches.error());
    // A title has no overrides: its record keeps the match it uses.
    const std::int64_t chosen =
        s.games.Contains(game->id)
            ? config::Resolver(s.config, game->overrides).GetInt("metadata.steamgriddb_id")
            : s.games.Metadata().Read(game->id).value("steamgriddb_id", std::int64_t{0});
    SendJson(res, {{"query", query}, {"chosen", chosen}, {"matches", *matches}});
  });

  http.Post(R"(/v1/games/([^/]+)/metadata/wrong-match)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    auto matches = metadata::SearchSteamGridDb(s.config, game->name);
    if (!matches) return SendError(res, 502, matches.error());
    const std::int64_t current =
        config::Resolver(s.config, game->overrides).GetInt("metadata.steamgriddb_id");
    std::size_t next = 1;  // no choice yet: the top match was in use
    for (std::size_t i = 0; i < matches->size(); ++i) {
      if ((*matches)[i].value("id", std::int64_t{0}) == current) next = i + 1;
    }
    if (next >= matches->size()) {
      return SendError(
          res, 409, "no_more_matches",
          "no other SteamGridDB match for this name -- pick one with ?q= on .../metadata/matches");
    }
    const json& match = (*matches)[next];
    const json patch = {{"metadata.steamgriddb_id", match.value("id", std::int64_t{0})}};
    auto updated = s.games.Update(
        game->id, [&](model::Game& g) { library::ApplyOverridesPatch(g, patch, s.config); });
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
    // A store title keeps its art: its picker lists the new match's, to pick from.
    if (!s.games.Contains(req.matches[1])) {
      if (!ArtSubject(s, req.matches[1]))
        return SendError(res, 404, "game_not_found", "no such game");
      if (auto matched = metadata::MatchTitle(s.games.Metadata(), req.matches[1], id); !matched)
        return SendError(res, 500, matched.error());
      s.events.Publish("game.metadata_ready", {{"id", req.matches[1]}});
      return SendJson(res, {{"status", "matched"}, {"steamgriddb_id", id}});
    }
    const json patch = {{"metadata.steamgriddb_id", id == 0 ? json(nullptr) : json(id)}};
    auto game = s.games.Update(
        req.matches[1], [&](model::Game& g) { library::ApplyOverridesPatch(g, patch, s.config); });
    if (!game) return SendStoreError(res, game.error());
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
    json unknown = json::array();
    for (const std::string& id : *ids) {
      if (auto game = s.games.Find(id))
        games.push_back(std::move(*game));
      else
        unknown.push_back(id);
    }
    if (games.empty()) return SendError(res, 404, "game_not_found", "no such game");
    s.StartJob(req, res, "metadata", "", "Refreshing metadata",
               [&s, games = std::move(games),
                unknown = std::move(unknown)](JobRegistry::Progress& progress) {
                 auto result = s.RefreshMetadata(games, progress);
                 if (result && !unknown.empty()) (*result)["unknown"] = unknown;
                 return result;
               });
  });

  http.Post("/v1/games/metadata/refresh-missing", [&s](const Request& req, Response& res) {
    std::vector<model::Game> games;
    for (const model::Game& game : s.games.All()) {
      if (!s.games.Metadata().ArtVersions(game.id).contains("cover")) games.push_back(game);
    }
    s.StartJob(req, res, "metadata", "", "Fetching missing cover art",
               [&s, games = std::move(games)](JobRegistry::Progress& progress) {
                 return s.RefreshMetadata(games, progress);
               });
  });
}

}  // namespace mira::api
