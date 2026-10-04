#pragma once

#include <QWidget>
#include <QString>

#include <map>
#include <string>
#include <vector>

#include <json.hpp>

#include "../client/MiradClient.h"

namespace mira_gui {
class KeyValueEdit;
class OverridesEditor;
class SettingRow;
class SettingsCard;
class TagEdit;
}

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace mira_gui {

// One game's editable record (GET/PATCH /v1/games/{id}) as Launch, Runner,
// Details and Files cards, shown in LibraryWindow's game card. Advanced
// swaps them for the runner options and the per-game overrides.
class GameEditForm : public QWidget {
  Q_OBJECT

public:
  explicit GameEditForm(std::string id, QWidget* parent = nullptr);

  const std::string& id() const { return id_; }

  // Offered while a new tag is typed.
  void SetTagSuggestions(const QStringList& tags);
  // Extra space under the last card, so a change bar floating over it never covers a row.
  void SetBottomRoom(int height);

  void OpenAdvanced();
  void CloseAdvanced();
  bool AdvancedOpen() const;

  void Save();
  // Puts every field back to what was last loaded or saved.
  void DiscardChanges();

  // True once any field differs from what Populate() last loaded or Save()
  // last confirmed; the signal a caller uses to warn before discarding.
  bool IsDirty() const;
  // How many fields and overrides differ, for the change bar.
  int ChangeCount() const;

signals:
  // The record loaded, or a save landed.
  void Loaded(QString name);
  void LoadFailed(QString error);
  void SaveFinished(bool ok, QString error);
  void AdvancedChanged(bool open);
  // Any field edited, or put back.
  void Changed();

private:
  void Load();
  void Populate(const mira_gui::GameDetail& game);
  void ShowPatch(const mira_gui::GamePatch& patch);
  void PopulateExeCombo(const std::vector<mira_gui::GameDetail::Candidate>& candidates,
                        const std::string& current);
  void PopulateRunnerCombo(const mira_gui::RunnersResult& result);
  // The kind of the runner picked (or the default it resolves to): "proton", "wine", ...
  std::string RunnerKind() const;
  // Lists the picked runner's options, fetching its schema the first time.
  void ShowRunnerOptions();
  void BuildRunnerFields(const std::vector<mira_gui::RunnerOption>& options);
  void ShowRunnerFieldValues();
  void OnExeComboActivated(int index);
  void BrowseExecutable();
  // POST /v1/games/{id}/relocate of the install folder or the prefix into a folder the user picks.
  void MoveFolder(bool prefix);
  // PATCH {"reviewed": true}: keeps mirad's pick of executable and clears its "Not checked".
  void ConfirmExecutable();
  void ShowInstallPath();
  void ResetScroll();
  void UpdateModified();
  mira_gui::GamePatch CurrentPatch() const;

  std::string id_;
  std::string install_path_;
  mira_gui::GamePatch original_patch_;
  bool populating_ = false;

  QLabel* last_error_label_ = nullptr;
  QLabel* source_note_label_ = nullptr;
  QLabel* check_tag_ = nullptr;
  QPushButton* looks_right_ = nullptr;
  QLineEdit* install_path_edit_ = nullptr;
  QPushButton* move_button_ = nullptr;
  QPushButton* open_data_dir_ = nullptr;
  QPushButton* move_prefix_ = nullptr;

  QLineEdit* name_edit_ = nullptr;
  QComboBox* exe_combo_ = nullptr;
  QLineEdit* args_edit_ = nullptr;
  QLineEdit* working_dir_edit_ = nullptr;
  TagEdit* tags_edit_ = nullptr;
  QComboBox* runner_combo_ = nullptr;
  QLineEdit* data_dir_edit_ = nullptr;
  KeyValueEdit* env_edit_ = nullptr;

  // Runner options: one row per key the chosen runner's schema lists, editing
  // runner_config_ (the whole object, so keys of other runners are kept).
  struct RunnerField {
    std::string key;
    SettingRow* row = nullptr;
    QLineEdit* edit = nullptr;
  };
  SettingsCard* runner_options_card_ = nullptr;
  std::vector<RunnerField> runner_fields_;
  std::map<std::string, std::vector<mira_gui::RunnerOption>> runner_schemas_;  // by kind
  std::string options_kind_;
  std::string default_runner_;  // what "Default runner" resolves to, from the game record
  nlohmann::json runner_config_ = nlohmann::json::object();

  SettingRow* name_row_ = nullptr;
  SettingRow* exe_row_ = nullptr;
  SettingRow* args_row_ = nullptr;
  SettingRow* working_dir_row_ = nullptr;
  SettingRow* tags_row_ = nullptr;
  SettingRow* runner_row_ = nullptr;
  SettingRow* data_dir_row_ = nullptr;
  SettingRow* env_row_ = nullptr;

  // The cards, then Advanced: the runner options and the per-game overrides.
  QStackedWidget* pages_ = nullptr;
  QVBoxLayout* cards_layout_ = nullptr;
  mira_gui::OverridesEditor* overrides_ = nullptr;
};

}  // namespace mira_gui
