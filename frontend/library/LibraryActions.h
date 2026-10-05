#pragma once

#include <QString>

#include <functional>
#include <string>

class QWidget;

// Actions on the whole library, each reporting its own outcome. The games
// they add or change arrive as events, so none relists.
namespace mira_gui::actions {

void ScanLibrary(QWidget* parent);
// `on_imported` runs once the import succeeded.
void ImportSteam(QWidget* parent, std::function<void()> on_imported);
void ImportLutris(QWidget* parent, std::function<void()> on_imported);
void SyncDesktopEntries(QWidget* parent);
// Asks first, then turns desktop entries off and removes them.
void RemoveAllDesktopEntries(QWidget* parent);
// Asks first. `game_name` names the games that failed to move.
void RelocateLibrary(QWidget* parent, std::function<QString(const std::string& id)> game_name);
// One job for every game without a cover; `on_finished` runs when it ends either way.
void FetchMissingArtwork(QWidget* parent, std::function<void()> on_finished);

}  // namespace mira_gui::actions
