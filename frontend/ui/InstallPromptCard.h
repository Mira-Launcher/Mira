#pragma once

#include <string>

#include "SettingsCard.h"

class QPushButton;

namespace mira_gui {

class ArtworkStore;
class Switch;
struct GameSummary;

// Asked after a launched game turned out to be an installer: switch the entry
// to the program it installed. An in-window card over the library.
class InstallPromptCard : public SettingsCard {
  Q_OBJECT

public:
  // `exe_path` is relative to `install_path`; empty when no program was found.
  InstallPromptCard(const GameSummary& game, const std::string& install_path, const std::string& exe_path,
                    ArtworkStore* artwork, QWidget* parent = nullptr);

signals:
  // Use `exe_path` (relative to the install folder) instead of the installer.
  void Accepted(std::string exe_path, bool is_app);
  // Keep the entry as it is.
  void CloseRequested();

private:
  void ShowProgram();
  void Choose();

  std::string install_path_;
  std::string exe_path_;
  QLabel* program_ = nullptr;
  Switch* app_ = nullptr;
  QPushButton* use_ = nullptr;
};

}  // namespace mira_gui
