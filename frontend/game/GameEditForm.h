#pragma once

#include <QString>
#include <QWidget>
#include <json.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {
class ExpanderRow;
class LaunchOptionsEdit;
class OverridesEditor;
class SettingRow;
class SettingsCard;
class TagEdit;
}  // namespace mira_gui

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace mira_gui {

// One game's editable record (GET/PATCH /v1/games/{id}) and its own settings, shown in
// LibraryWindow's game card as one column of cards for the kind of game it is: Details, then
// Steam, an Emulator, or Launch and Runner plus its store's card, then Files, then Overrides for
// the rest of the settings it can use.
class GameEditForm : public QWidget {
  Q_OBJECT

 public:
  explicit GameEditForm(std::string id, QWidget* parent = nullptr);

  const std::string& id() const { return id_; }

  // Offered while a new tag is typed.
  void SetTagSuggestions(const QStringList& tags);
  // The library's tags in the order the chips show (TagOrder).
  void SetTagOrder(std::vector<std::string> order);
  // Extra space under the last card, so a change bar floating over it never covers a row.
  void SetBottomRoom(int height);
  // The cards' column, centered in the form, as wide as Settings' (SettingsColumnWidth).
  void SetColumnWidth(int width);

  // Scrolls to `key`'s row, opening its expander.
  void RevealSetting(const std::string& key);

  void Save();
  // Fetches the game again, so a change made elsewhere shows; skipped while
  // there are unsaved edits, which it would otherwise overwrite.
  void Reload();
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
  void LoadFailed(QString error, bool first);
  void SaveFinished(bool ok, QString error);
  // Any field edited, or put back.
  void Changed();
  // A tag chip was clicked: show the library's games with this tag.
  void TagFilterRequested(QString tag);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  // The cards' margins: the column centered, and the bottom room under it.
  void FitColumn();
  // The batch PATCH /v1/games that turns the saved tags into `now`.
  mira_gui::GamesPatch TagsPatch(const std::optional<std::vector<std::string>>& now) const;
  void Load();
  // Puts the rows into the cards this kind of game has, the first time it loads.
  void Arrange(const mira_gui::GameDetail& game);
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
  std::string source_;
  QString noun_ = "game";  // or "app"
  mira_gui::GamePatch original_patch_;
  std::string original_pick_;  // the game's folder tag pick as saved
  bool populating_ = false;
  bool loaded_ = false;  // Populate() has run once
  bool stale_ = false;  // the game changed elsewhere while there were edits; Discard loads it again

  QLabel* last_error_label_ = nullptr;
  QLabel* check_tag_ = nullptr;
  QPushButton* looks_right_ = nullptr;
  QLineEdit* install_path_edit_ = nullptr;
  QPushButton* open_install_ = nullptr;
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

  // Runner options: one row per key the chosen runner's schema lists, editing
  // runner_config_ (the whole object, so keys of other runners are kept).
  struct RunnerField {
    std::string key;
    SettingRow* row = nullptr;
    QLineEdit* edit = nullptr;
  };
  ExpanderRow* runner_options_ = nullptr;
  std::vector<RunnerField> runner_fields_;
  std::map<std::string, std::vector<mira_gui::RunnerOption>> runner_schemas_;  // by kind
  std::string options_kind_;
  std::string default_runner_;  // what "Default runner" resolves to, from the game record
  nlohmann::json runner_config_ = nlohmann::json::object();

  SettingRow* name_row_ = nullptr;
  SettingRow* exe_row_ = nullptr;
  SettingRow* args_row_ = nullptr;
  SettingRow* working_dir_row_ = nullptr;
  SettingRow* launch_options_row_ = nullptr;
  LaunchOptionsEdit* launch_options_ = nullptr;
  SettingRow* play_starts_row_ = nullptr;
  SettingRow* only_choice_row_ = nullptr;
  bool windows_ = false;
  bool web_ = false;  // an HTML game: its runners are browsers, and only it runs in one
  mira_gui::RunnersResult runners_;  // the last list, kept to refill the picker once the game loads
  SettingRow* tags_row_ = nullptr;
  SettingRow* runner_row_ = nullptr;
  SettingRow* emulator_row_ = nullptr;  // in place of the runner for an emulator's game
  QComboBox* emulator_combo_ =
      nullptr;  // its emulator, then the others that play it: picking one moves it
  SettingRow* install_row_ = nullptr;
  SettingRow* data_dir_row_ = nullptr;
  bool arranged_ = false;

  QScrollArea* scroll_ = nullptr;
  QVBoxLayout* cards_layout_ = nullptr;
  int column_width_ = 0;  // 0 until set: the cards fill the form
  int bottom_room_ = 0;
  mira_gui::OverridesEditor* overrides_ = nullptr;
};

}  // namespace mira_gui
