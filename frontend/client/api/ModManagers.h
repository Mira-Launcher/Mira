#pragma once

#include <QObject>
#include <functional>
#include <string>

#include "../Types.h"

// /v1/mod-managers (docs/api.md, Mod managers).
namespace mira_gui::api {

void GetModManagersAsync(QObject* context, std::function<void(ModManagersResult)> callback);
// A job: downloads it and adds its source.
void InstallModManagerAsync(QObject* context, const std::string& id,
                            std::function<void(StoreActionResult)> callback);
// A job: readies the game's prefix for it, then adds it to the game as a tool.
void AddModManagerToGameAsync(QObject* context, const std::string& id, const std::string& game_id,
                              std::function<void(StoreActionResult)> callback);

}  // namespace mira_gui::api
