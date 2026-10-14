#pragma once

#include <QObject>
#include <functional>
#include <string>
#include <vector>

#include "../Types.h"

// Emulator sources: docs/api.md#emulators.
namespace mira_gui::api {

void GetEmulatorsAsync(QObject* context, std::function<void(EmulatorsResult)> callback);
// Downloads it from its releases page; a job.
void InstallEmulatorAsync(QObject* context, const std::string& id, std::function<void(StoreActionResult)> callback);
// Copies `paths` into its games folder, or moves them per emulators::MovesAdded, then imports them; a job.
void AddEmulatorFilesAsync(QObject* context, const std::string& id, const std::vector<std::string>& paths,
                           std::function<void(StoreImportResult)> callback);
void ImportEmulatorAsync(QObject* context, const std::string& id, std::function<void(StoreImportResult)> callback);
void AddEmulatorFirmwareAsync(QObject* context, const std::string& id, const std::string& firmware,
                              const std::vector<std::string>& paths, bool replace,
                              std::function<void(StoreActionResult)> callback);
void OpenEmulatorAsync(QObject* context, const std::string& id, std::function<void(StoreActionResult)> callback);

void GetExistingEmulatorGamesAsync(QObject* context, const std::string& id,
                                   std::function<void(ExistingEmulatorGamesResult)> callback);
// Makes them its games, moved into its games folder with `move`, else kept where they are; a job.
void AdoptEmulatorGamesAsync(QObject* context, const std::string& id, bool move,
                             std::function<void(StoreImportResult)> callback);

void GetGameEmulatorsAsync(QObject* context, const std::string& game_id,
                           std::function<void(EmulatorChoicesResult)> callback);
void MoveGameToEmulatorAsync(QObject* context, const std::string& game_id, const std::string& emulator,
                             std::function<void(StoreActionResult)> callback);

}  // namespace mira_gui::api
