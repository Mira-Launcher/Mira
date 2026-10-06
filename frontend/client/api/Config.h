#pragma once

#include <QImage>
#include <QObject>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../Types.h"

// Health, settings, the schema and frontend.toml.
// Each call runs on a worker thread and delivers its result on the main thread
// (client/Async.h), so the UI never blocks on the socket. See docs/api.md.
namespace mira_gui::api {

// The socket every call above goes to. Exposed because the UI shows it.
std::string ResolveSocketPath();

// GET /v1/health.
void CheckHealthAsync(QObject* context, std::function<void(HealthStatus)> callback);

// GET /v1/config/schema.
void GetConfigSchemaAsync(QObject* context, std::function<void(ConfigSchemaResult)> callback);

// GET /v1/config, flattened.
void GetConfigAsync(QObject* context, std::function<void(ConfigResult)> callback);

// PATCH /v1/config. A bad value anywhere in the patch means nothing
// in it is applied.
void PatchConfigAsync(QObject* context, const std::vector<ConfigEdit>& edits,
                      std::function<void(PatchConfigResult)> callback);

// POST /v1/config/reset?key=<dotted.key>.
void ResetConfigKeyAsync(QObject* context, const std::string& key,
                         std::function<void(PatchConfigResult)> callback);

// A config.changed event's prefs, read as GET /v1/config's are, and a fingerprint
// of its frontend table without the window layout the window saves itself, so an
// echo of that layout compares equal. Nothing when the payload has no table.
struct ChangedPrefs {
  FrontendPrefs prefs;
  std::string fingerprint;
};
std::optional<ChangedPrefs> ParseChangedPrefs(const std::string& payload);

// GET /v1/config, reading only the opaque `frontend` table.
void GetFrontendPrefsAsync(QObject* context, std::function<void(FrontendPrefsResult)> callback);

// PATCH /v1/config with a `frontend` key. Merge-patch, so only the fields
// set in `prefs` are written and a key this build doesn't know about
// (an older or newer frontend's) survives untouched.
void SaveFrontendPrefsAsync(QObject* context, const FrontendPrefs& prefs,
                            std::function<void(PatchConfigResult)> callback);

// The same PATCH, run on the calling thread.
//
// For the one caller that has nowhere to deliver a result to and no time
// to wait for one: a window saving its layout from closeEvent. The async
// form hands the request to a detached thread, which the process can
// outrun on its way out. One round trip over a Unix socket is cheap
// enough to just wait for, and the short timeout below means an
// unreachable daemon cannot turn quitting into a hang.
PatchConfigResult SaveFrontendPrefsBlocking(const FrontendPrefs& prefs);

// For the window's size before it's first shown, so the compositor places
// it at its real size.
FrontendPrefsResult GetFrontendPrefsBlocking();

// GET /v1/gamemode/status: whether the Feral GameMode daemon is installed
// and reachable. Purely a status check; see GameModeStatusResult.
void GetGameModeStatusAsync(QObject* context, std::function<void(GameModeStatusResult)> callback);

}  // namespace mira_gui::api
