#pragma once

#include <functional>
#include <json.hpp>
#include <string>

#include "saves/remote/RemoteConfig.h"

// Connecting a remote: signing in, checking it can be written to, and SSH's keys.
namespace mira::saves::remote {

// Checks that the remote can be reached and written to, then stores it in place of the slot's
// earlier one, which stays until then. Installs rclone when needed. For OneDrive and Dropbox the
// user signs in at the address `on_url` gets. `host_keys` are the ScanHost lines the user trusted.
Result<RemoteInfo> Connect(const config::Config& config, RemoteRequest request,
                           const std::string& host_keys,
                           const std::function<void(const std::string& url)>& on_url);

// Forgets the slot's connection and the host keys trusted for it; nothing on the remote changes.
Result<void> Disconnect(const config::Config& config, std::string_view slot);

// Tells whether a stored remote can be written to now.
Result<void> Probe(const config::Config& config, const Remote& remote);

// An SSH server's keys as it offers them now, for the user to compare:
// {host_keys, fingerprints: ["256 SHA256:... (ED25519)"]}.
Result<nlohmann::json> ScanHost(const std::string& host, const std::string& port);

// The SSH key Mira made for save remotes, made on first use: {path, public}.
Result<nlohmann::json> SshKey(const config::Config& config);

}  // namespace mira::saves::remote
