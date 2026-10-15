#pragma once

#include <string>

#include "../client/Types.h"
#include "../settings/SettingsCard.h"

namespace mira_gui {

// Asked when something was dropped into a Mira folder that Mira couldn't sort: where it goes.
// An in-window card over the library.
class UnclearDropCard : public SettingsCard {
  Q_OBJECT

 public:
  UnclearDropCard(const UnclearDrop& drop, QWidget* parent = nullptr);
  const std::string& Path() const { return path_; }

 signals:
  // The dropped path, sorted by choice `choice` (an id from the drop's choices).
  void Chosen(std::string path, std::string choice);
  // Asked again later.
  void CloseRequested();

 private:
  std::string path_;
};

}  // namespace mira_gui
