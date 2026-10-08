#include "Library.h"

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

GamesResult GetGamesSync(const std::string& status_filter, const std::string& tag_filter,
                         bool include_hidden) {
  std::string path = "/v1/games";
  std::string separator = "?";
  if (!status_filter.empty()) {
    path += separator + "status=" + status_filter;
    separator = "&";
  }
  if (!tag_filter.empty()) {
    path += separator + "tag=" + tag_filter;
    separator = "&";
  }
  if (include_hidden) path += separator + "include_hidden=true";
  return ReadReply<GamesResult>(transport::Get(path), "GET /v1/games", Shape::Array,
                                [](GamesResult& result, const json& body) {
                                  for (const json& entry : body) {
                                    result.games.push_back(mapping::ToGameSummary(entry));
                                  }
                                });
}

void FillScan(ScanResult& result, const json& body) {
  result.added = body.value("added", 0);
  result.missing = body.value("missing", 0);
  result.restored = body.value("restored", 0);
}

void FillLutrisImport(LutrisImportResult& result, const json& body) {
  result.added = body.value("added", 0);
  result.updated = body.value("updated", 0);
  result.other_runner = body.value("other_runner", 0);
  result.incomplete = body.value("incomplete", 0);
}

DesktopEntryCandidatesResult GetDesktopEntryCandidatesSync() {
  return ReadReply<DesktopEntryCandidatesResult>(
      transport::Get("/v1/desktop-entries/candidates"), "GET /v1/desktop-entries/candidates",
      Shape::Array, [](DesktopEntryCandidatesResult& result, const json& body) {
        for (const json& entry : body) {
          DesktopEntryCandidate c;
          c.id = entry.value("id", std::string());
          c.name = entry.value("name", std::string());
          c.icon = entry.value("icon", std::string());
          result.candidates.push_back(std::move(c));
        }
      });
}

DesktopEntryImportResult ImportDesktopEntriesSync(const std::vector<std::string>& ids) {
  return ReadReply<DesktopEntryImportResult>(
      transport::PostJson("/v1/desktop-entries/import", json{{"ids", ids}}),
      "POST /v1/desktop-entries/import", Shape::Object, FillAddedUpdated<DesktopEntryImportResult>);
}

DesktopEntrySyncResult SyncDesktopEntriesSync() {
  const transport::Reply reply = transport::Post("/v1/desktop-entries/sync");
  return {reply.ok, reply.error};
}

void FillRelocate(RelocateLibraryResult& result, const json& body) {
  result.moved = body.value("moved", 0);
  result.failed = body.value("failed", 0);
  result.errors = mapping::ToGameFailures(body, "errors");
}

// No `ids` relocates every game.
void RelocateLibraryJob(QObject* context, std::optional<std::vector<std::string>> ids,
                        std::function<void(RelocateLibraryResult)> callback) {
  RunJob<RelocateLibraryResult>(
      context, "relocate",
      [ids](const std::string& query) {
        return ids ? transport::PostJson("/v1/library/relocate" + query, {{"ids", *ids}})
                   : transport::Post("/v1/library/relocate" + query);
      },
      FillRelocate, std::move(callback));
}

}  // namespace

void ListAllGamesAsync(QObject* context, std::function<void(GamesResult)> callback) {
  async::Run(
      context, [] { return GetGamesSync(std::string(), std::string(), true); },
      std::move(callback));
}

void ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback) {
  RunJob<ScanResult>(
      context, "scan",
      [](const std::string& query) { return transport::Post("/v1/library/scan" + query); },
      FillScan, std::move(callback));
}

void ScanSteamAsync(QObject* context, std::function<void(SteamScanResult)> callback) {
  RunJob<SteamScanResult>(
      context, "scan",
      [](const std::string& query) { return transport::Post("/v1/steam/scan" + query); },
      FillAddedUpdated<SteamScanResult>, std::move(callback));
}

void SetSteamStatusAsync(QObject* context, const std::string& status,
                         std::function<void(StoreActionResult)> callback) {
  async::Run(
      context,
      [status] {
        const transport::Reply reply =
            transport::PostJson("/v1/steam/status", json{{"status", status}});
        return StoreActionResult{reply.ok, reply.error};
      },
      std::move(callback));
}

void ImportLutrisAsync(QObject* context, std::function<void(LutrisImportResult)> callback) {
  RunJob<LutrisImportResult>(
      context, "import",
      [](const std::string& query) { return transport::Post("/v1/lutris/import" + query); },
      FillLutrisImport, std::move(callback));
}

void GetDesktopEntryCandidatesAsync(QObject* context,
                                    std::function<void(DesktopEntryCandidatesResult)> callback) {
  async::Run(context, [] { return GetDesktopEntryCandidatesSync(); }, std::move(callback));
}

void ImportDesktopEntriesAsync(QObject* context, const std::vector<std::string>& ids,
                               std::function<void(DesktopEntryImportResult)> callback) {
  async::Run(context, [ids] { return ImportDesktopEntriesSync(ids); }, std::move(callback));
}

void SyncDesktopEntriesAsync(QObject* context,
                             std::function<void(DesktopEntrySyncResult)> callback) {
  async::Run(context, [] { return SyncDesktopEntriesSync(); }, std::move(callback));
}

void RelocateLibraryAsync(QObject* context, std::function<void(RelocateLibraryResult)> callback) {
  RelocateLibraryJob(context, std::nullopt, std::move(callback));
}

void RelocateGamesAsync(QObject* context, const std::vector<std::string>& ids,
                        std::function<void(RelocateLibraryResult)> callback) {
  RelocateLibraryJob(context, ids, std::move(callback));
}

void ListUnclearMovesAsync(QObject* context, std::function<void(UnclearMovesResult)> callback) {
  async::Run(
      context,
      [] {
        return ReadReply<UnclearMovesResult>(
            transport::Get("/v1/library/unclear"), "GET /v1/library/unclear", Shape::Object,
            [](UnclearMovesResult& result, const json& reply) {
              for (const json& entry : reply.value("moves", json::array())) {
                result.moves.push_back(mapping::ToUnclearMove(entry));
              }
            });
      },
      std::move(callback));
}

void SettleUnclearMoveAsync(QObject* context, const std::string& folder, const std::string& id,
                            std::function<void(SettleMoveResult)> callback) {
  async::Run(
      context,
      [folder, id] {
        json body = {{"folder", folder}};
        if (!id.empty()) body["id"] = id;
        return ReadReply<SettleMoveResult>(transport::PostJson("/v1/library/unclear", body), "POST /v1/library/unclear",
                                           Shape::Object, [](SettleMoveResult& result, const json& reply) {
                                             result.game = mapping::ToGameSummary(reply);
                                           });
      },
      std::move(callback));
}

}  // namespace mira_gui::api
