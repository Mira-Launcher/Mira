#pragma once

#include <QObject>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../Types.h"

// The library's tags as a whole: listing them with Steam's suggestions, and setting, renaming or
// removing one across every game (docs/api.md, Tags). Each call runs on a worker thread and
// delivers its result on the main thread (client/Async.h).
namespace mira_gui::api {

// GET /v1/tags.
void GetTagsAsync(QObject* context, std::function<void(TagsResult)> callback);

// POST /v1/tags/fetch: Steam tags for the games whose metadata has none yet, as a job.
void FetchSteamTagsAsync(QObject* context, std::function<void(SteamTagsFetchResult)> callback);

// POST /v1/tags/set: exactly the games in `ids` have `name` afterwards; `folder` also makes it a
// folder tag, or stops it being one.
void SetTagAsync(QObject* context, const std::string& name, const std::vector<std::string>& ids,
                 std::optional<bool> folder, std::function<void(PatchGamesResult)> callback);

// POST /v1/tags/rename, on every game and in the folder tags.
void RenameTagAsync(QObject* context, const std::string& from, const std::string& to,
                    std::function<void(PatchGamesResult)> callback);

// POST /v1/tags/remove: off every game and out of the folder tags.
void RemoveTagAsync(QObject* context, const std::string& name, std::function<void(PatchGamesResult)> callback);

// POST /v1/tags/preview: which games would move with these folder tags and sorted library folders
// (unset: as saved), counting `tags` (by game id) in place of those games' saved tags.
void PreviewTagsAsync(QObject* context, std::optional<std::vector<std::string>> folders,
                      std::optional<std::vector<std::string>> sorted_roots,
                      const std::map<std::string, std::vector<std::string>>& tags,
                      std::function<void(FolderTagsPreviewResult)> callback);

}  // namespace mira_gui::api
