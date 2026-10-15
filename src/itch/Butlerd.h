#pragma once

#include <functional>
#include <json.hpp>
#include <string>

#include "config/Config.h"
#include "core/Result.h"

// Minimal JSON-RPC 2.0 client for butlerd (itch.io's own
// launcher-integration daemon). `butler daemon --json
// --dbpath ...` prints a mix of its own JSON log lines and one
// butlerd/listen-notification line to stdout, then serves JSON-RPC 2.0
// over that TCP address, one message per line.
//
// Unlike every other source here, this is a long-lived daemon connection,
// not a one-shot subprocess per call -- Call() starts butler once,
// lazily, on first use, and keeps the connection for the rest of this
// mirad's run rather than paying its startup cost (subprocess spawn +
// database open) on every call.
namespace mira::itch {

// A butlerd notification: its method (e.g. "Progress") and params.
using NotificationHandler =
    std::function<void(const std::string& method, const nlohmann::json& params)>;

// One request/response round trip. `method`/`params` are butlerd's own
// (Meta.Authenticate, Profile.LoginWithAPIKey, Fetch.ProfileOwnedKeys,
// Fetch.Caves, Install.Queue/Perform, ...). Connects and authenticates
// the transport (Meta.Authenticate -- not the itch.io account, see
// itch::Login) on first use. A notification received while waiting for
// this call's response is skipped, not surfaced here.
Result<nlohmann::json> Call(const config::Config& config, const std::string& method,
                            const nlohmann::json& params);

// Like Call, with no practical deadline, for a long call (Install.Perform).
// Calls share butlerd's one connection concurrently, so this blocks nothing
// else. Notifications while it runs go to `on_notification`.
Result<nlohmann::json> CallLong(const config::Config& config, const std::string& method,
                                const nlohmann::json& params,
                                const NotificationHandler& on_notification = nullptr);

}  // namespace mira::itch
