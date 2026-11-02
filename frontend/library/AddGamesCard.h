#pragma once

#include <QPointer>
#include <QStringList>
#include <QWidget>
#include <optional>
#include <string>

#include "../client/Types.h"

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class SettingsCard;

// Adding games and apps by hand, as one in-window card (cards::Show): scanning the library
// folders, something already installed, an installer (added, then installed at once) or entries
// from the app menu. Each choice replaces the first page with its own, which has a way back.
// Steam, Lutris and Heroic aren't here: their sources add them.
class AddGamesCard : public QWidget {
  Q_OBJECT

 public:
  explicit AddGamesCard(QWidget* parent = nullptr);

  // Straight to the page for `path`, as a drop on the window does: Installer for an installer,
  // Already installed for anything else. A `name` replaces the one mirad guesses.
  void OpenPath(const QString& path, const QString& name = {});

 private:
  void ShowPage(SettingsCard* page);
  void ShowMenu();
  // Reads `path` (POST /v1/library/import/classify), then shows its page; `installer` picks the
  // Installer page whatever the file looks like, unset lets classify decide.
  void Classify(const QString& path, std::optional<bool> installer);
  void ShowInstalled(const QString& path, const ImportGuessResult& guess);
  void ShowInstaller(const QString& path, const ImportGuessResult& guess);
  void ShowAppMenu();
  void FillMenuDetails();
  void Close();
  // Where moving something in puts it: the first Mira folder's Games or Applications.
  QString MoveTarget(bool app) const;
  bool InLibraryRoot(const QString& path) const;

  QVBoxLayout* layout_ = nullptr;
  SettingsCard* page_ = nullptr;
  int request_ = 0;  // only the latest Classify's reply counts
  QStringList library_roots_;
  QString mira_folder_;
  QString name_hint_;  // OpenPath's name, for the page it opens
  std::optional<DesktopEntryCandidatesResult> candidates_;  // fetched as the card opens
  // The first page's lines that fill in once the config and the app menu are read.
  QPointer<QLabel> scan_detail_;
  QPointer<QLabel> menu_detail_;
  QPointer<QPushButton> menu_choose_;
};

}  // namespace mira_gui
