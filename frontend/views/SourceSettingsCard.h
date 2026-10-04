#pragma once

#include <QString>

#include <vector>

#include "../ui/SettingEditor.h"
#include "../ui/SettingsCard.h"
#include "SourcePage.h"

class QComboBox;
class QLabel;
class QPushButton;

namespace mira_gui {

// A source page's settings, opened from its banner: the runner its games
// use, then every setting under its own keys. Like the settings screen,
// edits apply on Save; "Switch them too" is an action and applies at once.
class SourceSettingsCard : public SettingsCard {
  Q_OBJECT

public:
  SourceSettingsCard(const SourceInfo& source, QWidget* parent = nullptr);

  // Reloads everything from mirad, e.g. after the launcher was installed.
  void Refresh();

  bool IsDirty() const;
  // Emits SaveFinished once mirad has answered.
  void Save();
  void Discard();

signals:
  // "All settings…": the full settings screen at `focus_key`.
  void OpenSettingsRequested(QString focus_key);
  void SaveFinished(bool ok);

private:
  bool HasRunner() const;
  bool RunnerDirty() const;
  void BuildRunnerRow();
  void LoadRunner();
  void ShowRunner(const SourceRunnerResult& runner);
  void SelectRunner(const QString& runner_ref);
  // The runner list from `runners`, with `pick` selected.
  void FillRunners(const RunnersResult& runners, const QString& pick);
  // A build was installed or removed: list them again in every runner picker here.
  void RunnersChanged();
  void LoadSettings();
  void UpdateButtons();
  void ShowStatus(const QString& text, bool error);
  void SaveRunner();
  void FinishSave(bool ok);

  SourceInfo source_;
  std::string id_;

  SettingRow* runner_row_ = nullptr;
  QComboBox* runner_ = nullptr;
  QLabel* runner_note_ = nullptr;
  QPushButton* runner_apply_ = nullptr;
  bool runner_can_apply_ = false;  // some games use another runner
  QString runner_ref_;  // what mirad last confirmed

  std::vector<SettingEditor> settings_;
  QWidget* footer_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* discard_ = nullptr;
  QPushButton* save_ = nullptr;
  bool saving_ = false;
  int saves_pending_ = 0;
  QString save_error_;
};

}  // namespace mira_gui
