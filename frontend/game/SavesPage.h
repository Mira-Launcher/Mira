#pragma once

#include <QWidget>
#include <optional>
#include <string>

#include "../client/Types.h"

class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;

namespace mira_gui {

class Card;

// The game card's Saves page: its backups, newest first, with Restore and Delete; where its saves
// are; and its saves settings. Controls, for the card's panel, holds Back up now and, for a GOG or
// Epic game, how they stand with the store's cloud. Restoring backs up what's there first, so it
// offers Undo rather than asking.
class SavesPage : public QWidget {
  Q_OBJECT

 public:
  explicit SavesPage(std::string id, QWidget* parent = nullptr);

  void Show(const SavesResult& saves);
  // What mirad is doing with the game's saves now (game.saves_activity), which holds off the
  // page's own actions until it's done.
  void SetDoing(const std::string& doing);
  // Back up now and the cloud's state, for the host to show beside the page.
  QWidget* Controls() const { return controls_; }

 signals:
  // Open the game's Advanced settings at `key`.
  void SettingClicked(std::string key);

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  void Arrange();
  void FillBackups();
  void FillWhere();
  void FillCloud();
  void LoadSettings();
  void BackUp();
  void Restore(const SaveSnapshot& snapshot);
  void Delete(const SaveSnapshot& snapshot);
  void Pin(const SaveSnapshot& snapshot);
  void Sync(const std::string& action);
  void AddFolder();
  void RemoveFolder(const std::string& pattern);
  // Whether a backup, restore or sync from here is running, which disables the others.
  void SetBusy(bool busy);
  bool Busy() const { return busy_ || !doing_.empty(); }
  // Moves focus off the page's buttons before they're disabled or rebuilt.
  void HoldFocus();

  std::string id_;
  SavesResult saves_;
  bool busy_ = false;
  // The newest LoadSettings: an older answer arriving after it is dropped.
  int settings_request_ = 0;
  std::string doing_;
  // After a restore: what was restored and the backup that undoes it, until the next action.
  struct Restored {
    std::int64_t taken_at = 0;
    std::string undo;
  };
  std::optional<Restored> restored_;

  QWidget* controls_ = nullptr;
  QPushButton* back_up_ = nullptr;
  QWidget* cloud_state_ = nullptr;  // in controls_: the cloud's state, when it was synced, Sync now
  QScrollArea* scroll_ = nullptr;
  QGridLayout* grid_ = nullptr;
  int columns_ = 0;
  Card* backups_ = nullptr;
  Card* where_ = nullptr;
  Card* cloud_ = nullptr;
  Card* settings_ = nullptr;
};

}  // namespace mira_gui
