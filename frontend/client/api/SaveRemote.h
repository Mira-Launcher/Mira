#pragma once

#include <QObject>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../ApiError.h"

// The user's own remote for saves (docs/api.md#your-own-remote).
namespace mira_gui::api {

struct SaveRemote {
  std::string provider;
  std::string name;
  std::string folder;
};

struct PendingSave {
  std::string id;
  std::string name;
  std::string slot;
  std::string state;
  std::string error;
};

struct SaveRemoteState {
  bool ok = false;
  ApiError error;
  std::optional<SaveRemote> primary;
  std::optional<SaveRemote> mirror;
  std::int64_t synced_at = 0;
  std::vector<PendingSave> pending;
};

struct SaveRemoteRequest {
  std::string slot;
  std::string provider;
  std::string name;
  std::string folder;
  std::string ca_file;
  std::string host_keys;
  std::map<std::string, std::string> values;
};

struct HostKeys {
  bool ok = false;
  ApiError error;
  std::string keys;
  std::vector<std::string> fingerprints;
};

struct SshKey {
  bool ok = false;
  ApiError error;
  std::string path;
  std::string public_key;
};

void GetSaveRemoteAsync(QObject* context, std::function<void(SaveRemoteState)> callback);
// Connects and checks it. Returns the job, for jobs::Cancel; the browser sign-in's address comes as
// the saves.remote_sign_in event.
std::string ConnectSaveRemoteAsync(QObject* context, const SaveRemoteRequest& request,
                                   std::function<void(SaveRemoteState)> callback);
void DisconnectSaveRemoteAsync(QObject* context, const std::string& slot,
                               std::function<void(SaveRemoteState)> callback);
// Syncs every game now. The state after is fetched anew.
void SyncAllSavesAsync(QObject* context, std::function<void(SaveRemoteState)> callback);
void ScanHostKeysAsync(QObject* context, const std::string& host, const std::string& port,
                       std::function<void(HostKeys)> callback);
void MakeSshKeyAsync(QObject* context, std::function<void(SshKey)> callback);

}  // namespace mira_gui::api
