#include "Artwork.h"

#include <QBuffer>
#include <QImageReader>

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

// `max` (when not empty) bounds the decoded size, keeping the aspect ratio.
QImage DecodeImage(const std::string& bytes, QSize max = {}) {
  QByteArray data = QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size()));
  QBuffer buffer(&data);
  QImageReader reader(&buffer);
  if (const QSize size = reader.size();
      !max.isEmpty() && size.isValid() && (size.width() > max.width() || size.height() > max.height())) {
    reader.setScaledSize(size.scaled(max, Qt::KeepAspectRatio));
  }
  QImage image;
  reader.read(&image);
  return image;
}

ArtworkResult GetArtworkSync(const std::string& id, const std::string& slot) {
  ArtworkResult result;
  const transport::Blob blob =
      transport::GetBinary("/v1/games/" + PercentEncode(id) + "/artwork?type=" + slot);
  if (blob.status == 404) {
    result.missing = true;
    return result;
  }
  if (!blob.ok) {
    result.error = blob.error;
    return result;
  }
  result.ok = true;
  result.bytes = blob.bytes;
  result.content_type = blob.content_type;
  return result;
}

void FillMetadata(GameMetadataResult& result, const json& body) {
  GameMetadata& out = result.metadata;
  out.source = body.value("source", std::string());

  const auto strings = [](const json& array) {
    std::vector<std::string> values;
    if (!array.is_array()) return values;
    for (const json& item : array) {
      if (item.is_string()) values.push_back(item.get<std::string>());
    }
    return values;
  };

  // Every block is optional: which ones mirad cached depends on the source,
  // and on what that source had for this game.
  if (body.contains("steam") && body["steam"].is_object()) {
    const json& steam = body["steam"];
    out.description = steam.value("short_description", std::string());
    out.release_date = steam.value("release_date", std::string());
    out.developers = strings(steam.value("developers", json::array()));
    out.genres = strings(steam.value("genres", json::array()));
    out.price = steam.value("price", std::string());
    out.metacritic_score = steam.value("metacritic_score", 0);
    out.website = steam.value("website", std::string());
    if (steam.contains("pc_requirements") && steam["pc_requirements"].is_object()) {
      const json& requirements = steam["pc_requirements"];
      out.requirements_min = requirements.value("minimum", std::string());
      out.requirements_rec = requirements.value("recommended", std::string());
    }
    if (steam.contains("dlc") && steam["dlc"].is_array()) {
      for (const json& item : steam["dlc"]) {
        if (item.is_number_integer()) out.dlc_ids.push_back(item.get<std::int64_t>());
      }
    }
    out.content_descriptors = strings(steam.value("content_descriptors", json::array()));
    out.achievements_total = steam.value("achievements_total", 0);
    out.controller_support = steam.value("controller_support", std::string());
    out.screenshots = strings(steam.value("screenshots", json::array()));
    out.trailers = strings(steam.value("movies", json::array()));
  }
  // From Legendary's catalog cache: only a description and a developer.
  if (body.contains("epic") && body["epic"].is_object()) {
    const json& epic = body["epic"];
    if (out.description.empty()) out.description = epic.value("description", std::string());
    if (const std::string developer = epic.value("developer", std::string());
        !developer.empty() && out.developers.empty()) {
      out.developers.push_back(developer);
    }
  }
  if (body.contains("steam_reviews") && body["steam_reviews"].is_object()) {
    const json& reviews = body["steam_reviews"];
    out.review_summary = reviews.value("score_description", std::string());
    out.review_total = reviews.value("total_reviews", 0);
    if (out.review_total > 0) {
      out.review_percent = (reviews.value("total_positive", 0) * 100 + out.review_total / 2) / out.review_total;
    }
  }
  if (body.contains("protondb") && body["protondb"].is_object()) {
    out.protondb_tier = body["protondb"].value("tier", std::string());
  }
  if (body.contains("steam_tags") && body["steam_tags"].is_object()) {
    out.steam_tags = strings(body["steam_tags"].value("tags", json::array()));
  }
  // A store title's details have no genres; its top Steam tags say as much.
  if (out.genres.empty()) out.genres.assign(out.steam_tags.begin(), out.steam_tags.begin() + std::min<std::size_t>(3, out.steam_tags.size()));
  // "artwork" is the cover slot under its pre-`hero` name; see docs/api.md.
  for (const char* key : {"artwork", "hero", "logo", "icon"}) {
    if (!body.contains(key) || !body[key].is_object()) continue;
    out.art_slots.push_back(std::string(key) == "artwork" ? "cover" : key);
  }

  for (const char* slot : {"cover", "hero", "logo", "icon"}) {
    if (body.contains("art_candidates") && body["art_candidates"].is_object() &&
        body["art_candidates"].contains(slot) && body["art_candidates"][slot].is_array()) {
      for (const json& item : body["art_candidates"][slot]) out.candidates[slot].push_back(mapping::ToArtCandidate(item));
    }
    const char* key = std::string_view(slot) == "cover" ? "artwork" : slot;
    if (body.contains(key) && body[key].is_object() && body[key].contains("candidate_id")) {
      out.active_candidate_ids[slot] = body[key].value("candidate_id", std::int64_t{0});
    }
  }
}

GameMetadataResult GetMetadataSync(const std::string& id) {
  const transport::Reply reply = transport::Get("/v1/games/" + PercentEncode(id) + "/metadata");
  if (reply.status == 404) {
    GameMetadataResult result;
    result.missing = true;
    return result;
  }
  return ReadReply<GameMetadataResult>(reply, "GET /v1/games/" + id + "/metadata", Shape::Object,
                                       FillMetadata);
}

MetadataRefreshResult RefreshMetadataSync(const std::string& id, bool announce) {
  const transport::Reply reply = transport::Post(
      "/v1/games/" + PercentEncode(id) + "/metadata/refresh?announce=" + (announce ? "1" : "0"));
  return {reply.ok, reply.error};
}

void FillMetadataBatch(MetadataBatchResult& result, const json& body) {
  result.refreshed = body.value("refreshed", 0);
  result.failed = body.value("failed", 0);
}

ArtworkSelectResult SelectArtworkSync(const std::string& id, const std::string& slot,
                                      std::int64_t candidate_id) {
  const transport::Reply reply =
      transport::PostJson("/v1/games/" + PercentEncode(id) + "/artwork?type=" + slot,
                          json{{"candidate_id", candidate_id}});
  return {reply.ok, reply.error};
}

ArtworkSelectResult UploadArtworkSync(const std::string& id, const std::string& slot, const std::string& bytes) {
  const std::string type = bytes.starts_with("\x89PNG") ? "image/png" : "image/jpeg";
  const transport::Reply reply = transport::Put("/v1/games/" + PercentEncode(id) + "/artwork?type=" + slot, bytes, type);
  return {reply.ok, reply.error};
}

GameActionResult FetchArtCandidatesSync(const std::string& id, const std::string& slot, int page,
                                        const std::string& request) {
  const transport::Reply reply =
      transport::Post("/v1/games/" + PercentEncode(id) + "/artwork/candidates?type=" + slot +
                      "&page=" + std::to_string(page) + "&request=" + request);
  return {reply.ok, reply.error};
}

GameActionResult FetchArtThumbsSync(const std::string& id, const std::string& slot,
                                    const std::vector<std::int64_t>& candidate_ids) {
  const transport::Reply reply =
      transport::PostJson("/v1/games/" + PercentEncode(id) + "/artwork/thumbs?type=" + slot,
                          json{{"candidate_ids", candidate_ids}});
  return {reply.ok, reply.error};
}

ArtThumbsResult GetArtThumbsSync(const std::string& id, const std::string& slot,
                                 const std::vector<std::int64_t>& candidate_ids) {
  ArtThumbsResult result;
  for (const std::int64_t candidate_id : candidate_ids) {
    const transport::Blob blob =
        transport::GetBinary("/v1/games/" + PercentEncode(id) + "/artwork/thumb?type=" + slot +
                             "&candidate_id=" + std::to_string(candidate_id));
    if (blob.ok) result.images.emplace_back(candidate_id, blob.bytes);
  }
  return result;
}

GriddbMatchesResult GetGriddbMatchesSync(const std::string& id, const std::string& query) {
  std::string url = "/v1/games/" + PercentEncode(id) + "/metadata/matches";
  if (!query.empty()) url += "?q=" + PercentEncode(query);
  return ReadReply<GriddbMatchesResult>(
      transport::Get(url, {.read_timeout = std::chrono::seconds(30)}),
      "GET /v1/games/" + id + "/metadata/matches", Shape::Object,
      [](GriddbMatchesResult& result, const json& body) {
        result.query = body.value("query", std::string());
        result.chosen = body.value("chosen", std::int64_t{0});
        for (const json& entry : body.value("matches", json::array())) {
          GriddbMatch match;
          match.id = entry.value("id", std::int64_t{0});
          match.name = entry.value("name", std::string());
          if (const std::int64_t released = entry.value("release_date", std::int64_t{0});
              released > 0) {
            const auto day = std::chrono::floor<std::chrono::days>(
                std::chrono::sys_seconds{std::chrono::seconds{released}});
            match.year = static_cast<int>(std::chrono::year_month_day{day}.year());
          }
          result.matches.push_back(std::move(match));
        }
      });
}

GameActionResult SetGriddbMatchSync(const std::string& id, std::int64_t griddb_id) {
  const transport::Reply reply = transport::PostJson(
      "/v1/games/" + PercentEncode(id) + "/metadata/match", {{"steamgriddb_id", griddb_id}});
  return {reply.ok, reply.error};
}

ArtworkResult GetTitleArtworkSync(const std::string& source, const std::string& ref) {
  ArtworkResult result;
  const transport::Blob blob = transport::GetBinary(
      "/v1/library/artwork?source=" + PercentEncode(source) + "&ref=" + PercentEncode(ref));
  if (blob.status == 404) {
    result.missing = true;
    return result;
  }
  if (!blob.ok) {
    result.error = blob.error;
    return result;
  }
  result.ok = true;
  result.bytes = blob.bytes;
  result.content_type = blob.content_type;
  return result;
}

}  // namespace

void ClearArtThumbsBlocking() {
  transport::Delete("/v1/artwork/thumbs", {.read_timeout = std::chrono::seconds(2)});
}

void GetArtworkImageAsync(QObject* context, const std::string& id, const std::string& slot,
                          std::function<void(QImage)> callback, QSize max) {
  async::Run<QImage>(
      context,
      [id, slot, max] {
        const ArtworkResult result = GetArtworkSync(id, slot);
        return result.ok ? DecodeImage(result.bytes, max) : QImage();
      },
      std::move(callback));
}

void GetMetadataAsync(QObject* context, const std::string& id,
                      std::function<void(GameMetadataResult)> callback) {
  async::Run(context, [id] { return GetMetadataSync(id); }, std::move(callback));
}

void GetTitleMetadataAsync(QObject* context, const std::string& source, const std::string& ref,
                           std::function<void(GameMetadataResult)> callback) {
  async::Run(
      context,
      [source, ref] {
        const transport::Reply reply =
            transport::Get("/v1/library/metadata?source=" + PercentEncode(source) + "&ref=" + PercentEncode(ref));
        if (reply.status == 404) {
          GameMetadataResult result;
          result.missing = true;
          return result;
        }
        return ReadReply<GameMetadataResult>(reply, "GET /v1/library/metadata", Shape::Object, FillMetadata);
      },
      std::move(callback));
}

void ClearGameArtThumbsAsync(QObject* context, const std::string& id) {
  async::Run<bool>(
      context, [id] { return transport::Delete("/v1/games/" + PercentEncode(id) + "/artwork/thumbs").ok; },
      [](bool) {});
}

void RefreshMetadataAsync(QObject* context, const std::string& id, bool announce,
                          std::function<void(MetadataRefreshResult)> callback) {
  async::Run(
      context, [id, announce] { return RefreshMetadataSync(id, announce); }, std::move(callback));
}

void RefreshMetadataManyAsync(QObject* context, const std::vector<std::string>& ids,
                              std::function<void(MetadataBatchResult)> callback) {
  const json body = {{"ids", ids}};
  RunJob<MetadataBatchResult>(
      context, "metadata",
      [body](const std::string& query) {
        return transport::PostJson("/v1/games/metadata/refresh" + query, body);
      },
      FillMetadataBatch, std::move(callback));
}

void SelectArtworkAsync(QObject* context, const std::string& id, const std::string& slot,
                        std::int64_t candidate_id,
                        std::function<void(ArtworkSelectResult)> callback) {
  async::Run(
      context, [id, slot, candidate_id] { return SelectArtworkSync(id, slot, candidate_id); },
      std::move(callback));
}

void UploadArtworkAsync(QObject* context, const std::string& id, const std::string& slot, std::string bytes,
                        std::function<void(ArtworkSelectResult)> callback) {
  async::Run(
      context, [id, slot, bytes = std::move(bytes)] { return UploadArtworkSync(id, slot, bytes); },
      std::move(callback));
}

void FetchArtCandidatesAsync(QObject* context, const std::string& id, const std::string& slot,
                             int page, const std::string& request,
                             std::function<void(GameActionResult)> callback) {
  async::Run(
      context,
      [id, slot, page, request] { return FetchArtCandidatesSync(id, slot, page, request); },
      std::move(callback));
}

void FetchArtThumbsAsync(QObject* context, const std::string& id, const std::string& slot,
                         const std::vector<std::int64_t>& candidate_ids,
                         std::function<void(GameActionResult)> callback) {
  async::Run(
      context, [id, slot, candidate_ids] { return FetchArtThumbsSync(id, slot, candidate_ids); },
      std::move(callback));
}

void GetArtThumbsAsync(QObject* context, const std::string& id, const std::string& slot,
                       const std::vector<std::int64_t>& candidate_ids,
                       std::function<void(std::vector<std::pair<std::int64_t, QImage>>)> callback) {
  async::Run<std::vector<std::pair<std::int64_t, QImage>>>(
      context,
      [id, slot, candidate_ids] {
        std::vector<std::pair<std::int64_t, QImage>> images;
        for (const auto& [candidate_id, bytes] : GetArtThumbsSync(id, slot, candidate_ids).images) {
          if (QImage image = DecodeImage(bytes); !image.isNull())
            images.emplace_back(candidate_id, std::move(image));
        }
        return images;
      },
      std::move(callback));
}

void RefreshMissingArtworkAsync(QObject* context,
                                std::function<void(MetadataBatchResult)> callback) {
  RunJob<MetadataBatchResult>(
      context, "metadata",
      [](const std::string& query) {
        return transport::Post("/v1/games/metadata/refresh-missing" + query);
      },
      FillMetadataBatch, std::move(callback));
}

void GetGriddbMatchesAsync(QObject* context, const std::string& id, const std::string& query,
                           std::function<void(GriddbMatchesResult)> callback) {
  async::Run(
      context, [id, query] { return GetGriddbMatchesSync(id, query); }, std::move(callback),
      async::Lane::Slow);
}

void SetGriddbMatchAsync(QObject* context, const std::string& id, std::int64_t griddb_id,
                         std::function<void(GameActionResult)> callback) {
  async::Run(
      context, [id, griddb_id] { return SetGriddbMatchSync(id, griddb_id); }, std::move(callback));
}

ArtworkResult GetArtworkBlocking(const std::string& id, const std::string& slot) {
  return GetArtworkSync(id, slot);
}

ArtworkResult GetTitleArtworkBlocking(const std::string& source, const std::string& ref) {
  return GetTitleArtworkSync(source, ref);
}

}  // namespace mira_gui::api
