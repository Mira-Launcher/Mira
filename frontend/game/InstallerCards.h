#pragma once

#include <cstdint>
#include <string>

#include "../client/Types.h"
#include "../settings/SettingsCard.h"

class QComboBox;
class QLabel;
class QPushButton;

namespace mira_gui {

class ArtworkStore;
struct GameSummary;

// Runs a needs_install game's installer (POST /v1/games/{id}/install): the installer it
// found, Change… to pick another, the library root to install into for one on a disc, and
// whether to run it quietly or with its window. An in-window card over the library, asked
// when one is found, from "Install…" and from an install error's fix.
class InstallerCard : public SettingsCard {
  Q_OBJECT

 public:
  // `info` is the game's own installer, read before the card shows so its rows are all there.
  InstallerCard(const GameSummary& game, const InstallerInfoResult& info, ArtworkStore* artwork,
                QWidget* parent = nullptr);

 signals:
  // mirad started the install; its progress shows in Activity and on the tile.
  void Started();
  void CloseRequested();

 private:
  void LoadInfo(const std::string& path);
  void ShowInfo(const InstallerInfoResult& info, const std::string& path);
  void Install(bool interactive);

  std::string game_id_;
  std::string install_path_;
  std::string installer_;        // empty: the game's own
  int info_request_ = 0;         // only the latest LoadInfo's reply counts
  bool silent_ = false;          // the loaded installer can run without its window
  QComboBox* target_ = nullptr;  // only for an installer outside the library roots
  QLabel* file_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* shown_ = nullptr;
  QPushButton* quiet_ = nullptr;
};

// After an install left its installer folder behind (game.installer_leftover): delete it
// (DELETE /v1/games/{id}/installer) or keep it.
class InstallerLeftoverCard : public SettingsCard {
  Q_OBJECT

 public:
  InstallerLeftoverCard(const GameSummary& game, const std::string& installer_dir,
                        std::int64_t bytes, ArtworkStore* artwork, QWidget* parent = nullptr);

 signals:
  void CloseRequested();

 private:
  std::string game_id_;
  QLabel* error_ = nullptr;
  QPushButton* delete_ = nullptr;
};

}  // namespace mira_gui
