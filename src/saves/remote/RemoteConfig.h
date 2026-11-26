#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "config/Config.h"
#include "core/Result.h"
#include "saves/remote/Rclone.h"

namespace mira::saves::remote {

// What may be shown of a connection; never a secret.
struct RemoteInfo {
  std::string slot;      // "primary" or "mirror"
  std::string id;        // random, new for each connection
  std::string provider;  // "onedrive", "dropbox", "webdav", "sftp" or "s3"
  std::string name;      // shown to the user
  std::string folder{};  // path in the remote; for s3 it starts with the bucket
  std::string ca_file{};
};

// A connection as the user typed it; values hold the provider's settings and secrets.
struct RemoteRequest {
  std::string slot{};
  std::string provider{};
  std::string name{};
  std::string folder{};
  std::string ca_file{};
  std::map<std::string, std::string> values{};
};

// Whether a provider is signed in to in a browser, rather than typed in.
bool SignsIn(std::string_view provider);
bool SftpHost(const std::string& host);
bool ValidPort(const std::string& port);

// Refuses a request that isn't safe to store, with invalid_remote.
Result<void> CheckRequest(const RemoteRequest& request);

// Mira's rclone config file; remotes are described in remotes.json beside it.
std::filesystem::path RcloneConfigFile(const config::Config& config);

// Stores the slot's connection, replacing any earlier one for that slot, once `check` (if any)
// passed on a private copy of its settings.
Result<RemoteInfo> SaveRemote(const config::Config& config, const RemoteRequest& request,
                              const std::function<Result<void>(const Remote&)>& check = {});

// The slot's connection, or nullopt when there is none.
std::optional<RemoteInfo> FindRemote(const config::Config& config, std::string_view slot);

// Removes the slot's connection; not an error when there is none.
Result<void> ForgetRemote(const config::Config& config, std::string_view slot);

// Where rclone reaches the connection.
Remote RemoteFor(const config::Config& config, const RemoteInfo& info);

}  // namespace mira::saves::remote
