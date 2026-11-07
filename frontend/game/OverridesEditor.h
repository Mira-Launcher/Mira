#pragma once

#include <QObject>
#include <map>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../settings/SettingEditor.h"

class QLabel;
class QPushButton;
class QWidget;

namespace mira_gui {

class ExpanderRow;
class SettingsCard;

// A game's own values for the settings it can use, resolved through default -> Settings -> this
// game and edited with the settings screen's own editors. A row the game sets for itself shows a
// dot, and its undo puts it back on Settings' value (an unsaved edit is undone first).
//
// The host places the rows that belong with its own cards (a store's settings, the environment
// variables); every other row goes into the Overrides card, one expander per category.
//
// Its own object because it talks to a different endpoint than the form around it: the form saves
// fields via PATCH .../games/{id}, this saves keys via PATCH .../games/{id}/config.
class OverridesEditor : public QObject {
  Q_OBJECT

 public:
  OverridesEditor(std::string game_id, QWidget* parent);

  // Puts `key`'s row into `card` at row `at` (-1 the end) instead of the Overrides card. Call
  // before Load.
  void PlaceRow(const std::string& key, SettingsCard* card, int at = -1);
  // The card the other rows go into. Call before Load.
  void SetOverridesCard(SettingsCard* card);
  // Shows `card`, which holds only placed rows, only while one of them is shown.
  void HideWhenEmpty(SettingsCard* card);

  // Fetches the schema and Settings' values the first time, then this game's values. Non-fatal if
  // a request fails: the rest of the form still works without these rows.
  void Load();

  // The keys changed or put back on Settings, ready for PATCH /v1/games/{id}/config. Empty when
  // nothing was touched, so a card opened and saved unedited sends no override patch at all.
  std::vector<GameConfigEdit> PendingEdits() const;
  // After PendingEdits() was applied: the edits are the new baseline, then the layers reload.
  void MarkSaved();
  void DiscardChanges();

  // The value `key`'s row holds now, empty before it is built.
  std::string Text(const std::string& key) const;
  // Hides `key`'s row whatever the game can use, e.g. environment variables Steam won't pass on.
  void SetRowHidden(const std::string& key, bool hidden);
  // Opens `key`'s expander and asks for its row to be scrolled to, once the values have arrived.
  void RevealKey(const std::string& key);

 signals:
  void Changed();
  void RevealRequested(QWidget* row);

 private:
  struct Field : SettingEditor {
    std::string layer;                 // "default" | "config" | "game"
    std::string settings;              // the value Settings gives it, what undo puts back
    bool applies = false;              // overridable and usable by this game
    bool hidden = false;               // SetRowHidden
    bool clearing = false;             // undo put it back on Settings; saved as a clear
    bool unlocked = false;             // Edit… pressed on a list the game took from Settings
    QPushButton* edit_list = nullptr;  // Edit… on a list the game takes from Settings
    QLabel* from_settings = nullptr;   // says so beside the label
    ExpanderRow* expander = nullptr;   // in the Overrides card, else placed by the host
    SettingsCard* card = nullptr;      // where the host placed it
    bool embedded = false;             // shown inside its shown_in's row, which undoes both
  };

  void BuildRows(const ConfigSchemaResult& schema);
  void ApplyValues(const GameConfigResult& config);
  void Reload();
  // Pressing undo on `index`'s row, for it and the settings shown inside it: the unsaved edits
  // first, then the game's own values.
  void Undo(size_t index);
  const Field* Find(const std::string& key) const;  // null for an empty or unknown key
  Field* Find(const std::string& key);
  // The game's own value, saved or about to be.
  bool Own(const Field& field) const;
  void ShowState(Field& field);
  void UpdateSummaries();

  struct Placement {
    SettingsCard* card = nullptr;
    int at = -1;
  };
  std::string game_id_;
  QWidget* parent_ = nullptr;
  std::map<std::string, Placement> placed_;
  SettingsCard* overrides_card_ = nullptr;
  std::vector<SettingsCard*> hide_when_empty_;
  std::map<std::string, ExpanderRow*> expanders_;  // by category
  std::map<std::string, std::string> settings_values_;
  std::string reveal_key_;  // a row RevealKey asked for before the values arrived
  bool values_loaded_ = false;
  bool loading_ = false;
  std::vector<Field> fields_;
};

}  // namespace mira_gui
