#include "SaveRemote.h"

#include <json.hpp>
#include <utility>

#include "../Async.h"
#include "../JsonMapping.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
namespace {

using nlohmann::json;

std::optional<SaveRemote> RemoteOf(const json& body, const char* slot) {
  const auto found = body.find(slot);
  if (found == body.end() || !found->is_object()) return std::nullopt;
  return SaveRemote{.provider = mapping::Str(*found, "provider"),
                    .name = mapping::Str(*found, "name"),
                    .folder = mapping::Str(*found, "folder")};
}

void Fill(SaveRemoteState& state, const json& body) {
  state.primary = RemoteOf(body, "primary");
  state.mirror = RemoteOf(body, "mirror");
  state.synced_at = body.value("synced_at", std::int64_t{0});
  for (const json& game : body.value("pending", json::array()))
    state.pending.push_back({.id = mapping::Str(game, "id"),
                             .name = mapping::Str(game, "name"),
                             .slot = mapping::Str(game, "slot"),
                             .state = mapping::Str(game, "state"),
                             .error = mapping::Str(game, "error")});
}

SaveRemoteState StateOf(const transport::Reply& reply) {
  return ReadReply<SaveRemoteState>(reply, "/v1/saves/remote", Shape::Object, Fill);
}

}  // namespace

void GetSaveRemoteAsync(QObject* context, std::function<void(SaveRemoteState)> callback) {
  async::Run(
      context, [] { return StateOf(transport::Get("/v1/saves/remote")); }, std::move(callback));
}

std::string ConnectSaveRemoteAsync(QObject* context, const SaveRemoteRequest& request,
                                   std::function<void(SaveRemoteState)> callback) {
  json body = {{"provider", request.provider},   {"name", request.name},
               {"folder", request.folder},       {"ca_file", request.ca_file},
               {"host_keys", request.host_keys}, {"values", request.values}};
  return RunJob<SaveRemoteState>(
      context, "saves",
      [slot = request.slot, text = body.dump()](const std::string& query) {
        return transport::Put("/v1/saves/remote/" + slot + query, text, "application/json");
      },
      Fill, std::move(callback));
}

void DisconnectSaveRemoteAsync(QObject* context, const std::string& slot,
                               std::function<void(SaveRemoteState)> callback) {
  async::Run(
      context, [slot] { return StateOf(transport::Delete("/v1/saves/remote/" + slot)); },
      std::move(callback));
}

void SyncAllSavesAsync(QObject* context, std::function<void(SaveRemoteState)> callback) {
  RunJob<SaveRemoteState>(
      context, "saves",
      [](const std::string& query) { return transport::Post("/v1/saves/remote/sync" + query); },
      [](SaveRemoteState&, const json&) {}, std::move(callback));
}

void ScanHostKeysAsync(QObject* context, const std::string& host, const std::string& port,
                       std::function<void(HostKeys)> callback) {
  RunJob<HostKeys>(
      context, "saves",
      [host, port](const std::string& query) {
        return transport::PostJson("/v1/saves/remote/host-keys" + query,
                                   json{{"host", host}, {"port", port}});
      },
      [](HostKeys& result, const json& body) {
        result.keys = mapping::Str(body, "host_keys");
        for (const json& print : body.value("fingerprints", json::array()))
          result.fingerprints.push_back(print.get<std::string>());
      },
      std::move(callback));
}

void MakeSshKeyAsync(QObject* context, std::function<void(SshKey)> callback) {
  async::Run(
      context,
      [] {
        return ReadReply<SshKey>(transport::Post("/v1/saves/remote/ssh-key"),
                                 "/v1/saves/remote/ssh-key", Shape::Object,
                                 [](SshKey& key, const json& body) {
                                   key.path = mapping::Str(body, "path");
                                   key.public_key = mapping::Str(body, "public");
                                 });
      },
      std::move(callback));
}

}  // namespace mira_gui::api
