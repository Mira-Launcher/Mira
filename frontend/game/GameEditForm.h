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

class Badge;
class ColumnGrid;
class ExpanderRow;
class LaunchOptionsEdit;
class OverridesEditor;
class SettingRow;
class Card;
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
// LibraryWindow's game card as cards in two columns (CardColumns): how it runs (Steam, an
// Emulator, or a Runner and its store's card, then a card per settings group) and the game
// itself (Details, Launch, Saves, Scripts, Files).
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
  // A column of cards' width; two fit side by side when there's room, else one.
  void SetColumnWidth(int width);

  // The cards under each heading of the card's jump list, in order, hidden ones included.
  struct JumpGroup {
    QString title;
    std::vector<Card*> cards;
  };
  std::vector<JumpGroup> Jumps() const;
  QScrollArea* Scroll() const { return scroll_; }

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
  // Cards came, showed or hid, or the count of what the game sets in one changed.
  void CardsChanged();

 private:
  // The cards' margins, with the bottom room under them.
  void FitColumn();
  // A card titled `title`, in its place in the game's card columns.
  Card* AddCard(const QString& title);
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
  // Points the game (or its prefix) at the folder or file the user picks, moving nothing.
  void ChangeFolder(bool prefix);
  void ShowDataDir();
  // POST /v1/games/{id}/detect: rebuilds the list of programs from the game's folder.
  void DetectExecutable();
  void ShowInstallPath();
  void ResetScroll();
  void UpdateModified();
  mira_gui::GamePatch CurrentPatch() const;

  std::string id_;
  std::string install_path_;
  std::string data_dir_;
  std::string browse_start_;  // where Browse… opens: drive C for a game an installer put there
  std::string source_;
  QString noun_ = "game";  // or "app"
  mira_gui::GamePatch original_patch_;
  std::string original_pick_;  // the game's folder tag pick as saved
  bool populating_ = false;
  bool loaded_ = false;  // Populate() has run once
  bool stale_ = false;  // the game changed elsewhere while there were edits; Discard loads it again

  QLabel* last_error_label_ = nullptr;
  QWidget* missing_ = nullptr;  // a missing game's way to point it at where its files went
  QLabel* missing_text_ = nullptr;
  QPushButton* detect_ = nullptr;
  QLineEdit* install_path_edit_ = nullptr;
  QPushButton* open_install_ = nullptr;
  QPushButton* change_install_ = nullptr;
  QPushButton* open_data_dir_ = nullptr;
  QPushButton* change_data_dir_ = nullptr;

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
  std::map<std::string, std::string> runner_defaults_;  // runner option -> what applies unset
  SettingRow* identity_row_ = nullptr;                  // which game it is (library::Identify)
  QLabel* identity_text_ = nullptr;
  // Whether the runner has the game's protonfix, and a build that does when it doesn't.
  SettingRow* fix_row_ = nullptr;
  QLabel* fix_text_ = nullptr;
  QPushButton* fix_use_ = nullptr;
  bool fix_known_ = false;
  bool unidentified_ = false;  // a Windows game Mira can't tell, so no fix can be found
  bool fix_applies_ = false;
  std::string fix_build_;
  void ShowFixStatus();
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
  QVBoxLayout* cards_layout_ = nullptr;  // the grid, inside the margins
  ColumnGrid* grid_ = nullptr;
  std::vector<QString> runs_cards_;  // CardColumns' columns, once Arrange knows the kind
  std::vector<QString> game_cards_;
  std::vector<Card*> cards_;  // in the order made
  int bottom_room_ = 0;
  mira_gui::OverridesEditor* overrides_ = nullptr;
};

}  // namespace mira_gui
