#pragma once

#include <string>

#include "../client/Types.h"
#include "../settings/SettingsCard.h"

namespace mira_gui {

class ArtworkStore;
class GameLibraryModel;

// Asked when a folder turned up in a library folder that could be any of several games moved by
// hand: which one it is, or a new game. An in-window card over the library.
class UnclearMoveCard : public SettingsCard {
  Q_OBJECT

public:
  UnclearMoveCard(const UnclearMove& move, const GameLibraryModel* library, ArtworkStore* artwork,
                  QWidget* parent = nullptr);
  const std::string& Folder() const { return folder_; }

signals:
  // The folder is game `id`'s; empty for a new game.
  void Chosen(std::string folder, std::string id);
  // Asked again later.
  void CloseRequested();

private:
  std::string folder_;
};

}  // namespace mira_gui
