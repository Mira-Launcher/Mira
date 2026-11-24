#pragma once

#include <QWidget>
#include <cstdint>
#include <optional>
#include <string>

#include "../client/Types.h"

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;
class Badge;
class Button;
class Segmented;
class SettingRow;
class SheetCard;
class Switch;

// Asks what a new game is before it's set up: which title, its name, the program, whether that
// starts or installs it, and game or app. For a game that isn't set up, and for an archive found in
// a library or Mira folder, asked before it's extracted. A card over the library; "Change…" turns
// it to a page of titles and back.
class GameSetupCard : public QWidget {
  Q_OBJECT

 public:
  // `installer` is read before the card shows, when the game waits on one.
  GameSetupCard(const GameDetail& game, const InstallerInfoResult& installer, ArtworkStore* artwork,
                QWidget* parent = nullptr);
  GameSetupCard(const ArchiveContentsResult& archive, ArtworkStore* artwork,
                QWidget* parent = nullptr);

 signals:
  // Answered, or put off.
  void CloseRequested();

 private:
  struct Program {
    QString path;  // relative to the game's folder or the archive, or absolute once browsed to
    qint64 bytes = -1;
    bool installer = false;
  };

  SheetCard* NewPage();
  void Build(const QString& subtitle);
  void AddArchiveRows();
  void AddTitleRow();
  void AddProgramRow();
  void AddInstallRows();
  void AddButtons();

  void ShowTitle();
  void IdentifyByName();
  // Takes the title mirad has found since.
  void Identify();
  void LoadFacts();
  void OpenTitles();
  void CloseTitles();
  void SetPrograms(const std::vector<Program>& programs);
  std::vector<Program> GamePrograms() const;
  void Browse();
  void LookAgain();
  void LoadInstaller();
  void ShowArchive(const ArchiveContentsResult& contents);
  void OpenWithPassword();
  void Refresh();
  QString ChosenProgram() const;
  bool Installs() const;
  SetupChoices Choices() const;
  // Saves the answers; `interactive` then also runs the installer, shown or not.
  void Confirm(std::optional<bool> interactive);
  void Extract();
  void Fail(const QString& what, const ApiError& why);
  void SetBusy(bool busy);

  bool IsArchive() const { return !archive_.archive.path.empty(); }

  GameDetail game_;
  ArchiveContentsResult archive_;
  InstallerInfoResult installer_;
  ArtworkStore* artwork_ = nullptr;
  std::vector<Program> programs_;
  bool installed_ = false;  // an installer put it in its prefix: its programs show as a list
  bool busy_ = false;
  bool picked_kind_ = false;   // Starts it or Installs it was clicked
  int request_ = 0;            // the latest title search
  int installer_request_ = 0;  // the latest look at an installer
  std::string password_;
  // The title: what it's identified as, or what was picked (0: not on Steam).
  std::int64_t appid_ = 0;
  QString title_name_;
  QString title_facts_;
  std::optional<std::int64_t> picked_appid_;

  QVBoxLayout* layout_ = nullptr;
  SheetCard* page_ = nullptr;
  SheetCard* titles_ = nullptr;
  QLabel* heading_ = nullptr;
  QLabel* problem_ = nullptr;
  QLabel* error_ = nullptr;
  QLabel* archive_facts_ = nullptr;
  SettingRow* password_row_ = nullptr;
  QLineEdit* password_edit_ = nullptr;
  SettingRow* title_row_ = nullptr;
  QLabel* cover_ = nullptr;
  QLabel* title_line_ = nullptr;
  QPushButton* change_title_ = nullptr;
  QLineEdit* name_ = nullptr;
  Badge* generic_ = nullptr;
  QComboBox* program_ = nullptr;
  QButtonGroup* program_list_ = nullptr;
  QVBoxLayout* program_list_layout_ = nullptr;
  Segmented* does_ = nullptr;
  QLabel* does_hint_ = nullptr;
  SettingRow* installer_row_ = nullptr;
  QLabel* installer_line_ = nullptr;
  SettingRow* target_row_ = nullptr;
  QComboBox* target_ = nullptr;
  Segmented* kind_ = nullptr;
  Switch* delete_archive_ = nullptr;
  QLabel* foot_ = nullptr;
  QPushButton* other_ = nullptr;  // Look again, or Another app is extracting it
  Button* shown_ = nullptr;
  Button* main_ = nullptr;
};

}  // namespace mira_gui
