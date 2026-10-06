#pragma once

#include <QDialog>
#include <QStringList>

#include <map>
#include <vector>

#include "../client/Types.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;

namespace mira_gui {

// The first launch's setup: what Mira is for, which folder it manages, which
// sources and a few looks. Writes mirad's settings and the frontend prefs when
// finished, and sets `onboarded` however it is left, so it asks once.
class FirstRunWizard : public QDialog {
  Q_OBJECT

public:
  // `prefs` fills the choices in, which is how running it again keeps what was set.
  explicit FirstRunWizard(const FrontendPrefs& prefs, QWidget* parent = nullptr);

  // Whether to show it for a launch with these prefs.
  static bool Needed(const FrontendPrefs& prefs);

protected:
  void reject() override;

private:
  enum Page { Welcome, Use, Folder, Sources, Runner, Art, Sidebar, Check, Summary, PageCount };

  QWidget* BuildWelcome();
  QWidget* BuildUse();
  QWidget* BuildFolder();
  QWidget* BuildSources();
  QWidget* BuildRunner();
  QWidget* BuildArt();
  QWidget* BuildSidebar();
  QWidget* BuildCheck();
  QWidget* BuildSummary();

  void Go(int delta);
  void ShowPage(int page);
  QString PrimaryUse() const;
  void PreselectSources();
  void FillCheck();
  QString SummaryText() const;
  std::vector<ConfigEdit> Edits() const;
  FrontendPrefs Prefs() const;
  void Finish();
  void Skip();

  FrontendPrefs initial_;
  QStackedWidget* pages_ = nullptr;
  QLabel* step_ = nullptr;
  QLabel* error_ = nullptr;
  QPushButton* back_ = nullptr;
  QPushButton* next_ = nullptr;
  QPushButton* skip_ = nullptr;

  QButtonGroup* use_ = nullptr;
  QButtonGroup* folder_ = nullptr;
  QLineEdit* folder_path_ = nullptr;
  std::map<QString, QCheckBox*> sources_;
  QLineEdit* steam_root_ = nullptr;
  QLineEdit* steam_key_ = nullptr;
  QLineEdit* steam_id_ = nullptr;
  QCheckBox* get_runner_ = nullptr;
  QLabel* runner_note_ = nullptr;
  QCheckBox* metadata_ = nullptr;
  QLineEdit* art_key_ = nullptr;
  QComboBox* pinned_style_ = nullptr;
  QComboBox* recent_style_ = nullptr;
  QSpinBox* recent_count_ = nullptr;
  QLabel* check_ = nullptr;
  QLabel* summary_ = nullptr;

  bool have_runners_ = true;  // until mirad says otherwise
  bool saving_ = false;
  bool done_ = false;
};

}  // namespace mira_gui
