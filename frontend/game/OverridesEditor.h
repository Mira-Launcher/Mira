#pragma once

#include <QObject>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../settings/SettingEditor.h"

class QLabel;
class QPushButton;
class QWidget;

namespace mira_gui {

class Card;

// A game's own values for the settings it can use, resolved through default -> Settings -> this
// game and edited with the settings screen's own editors. A row the game sets for itself shows a
// dot, and its undo puts it back on Settings' value (an unsaved edit is undone first).
//
// The host places the rows that belong with its own cards, one by one (a store's settings) or a
// schema group at a time (the environment variables in Launch); every other group gets a card of
// its own from the host's card maker. A card shows only while it has a row this game can use, and
// says in its header how many of them the game sets for itself.
//
// Its own object because it talks to a different endpoint than the form around it: the form saves
// fields via PATCH .../games/{id}, this saves keys via PATCH .../games/{id}/config.
class OverridesEditor : public QObject {
  Q_OBJECT

 public:
  OverridesEditor(std::string game_id, QWidget* parent);

  // Puts `key`'s row into `card` at row `at` (-1 the end) instead of the Overrides card. Call
  // before Load.
  void PlaceRow(const std::string& key, Card* card, int at = -1);
  // Puts the rows of the schema group titled `group_label` into `card`. Call before Load.
  void PlaceGroup(const std::string& group_label, Card* card);
  // Makes the card for a group nothing placed, titled `group_label`. Call before Load.
  void SetCardMaker(std::function<Card*(const std::string& group_label)> maker);
  // Shows `card`, which holds only placed rows, only while one of them is shown.
  void HideWhenEmpty(Card* card);

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
  // Asks for `key`'s row to be scrolled to, once the values have arrived.
  void RevealKey(const std::string& key);

 signals:
  void Changed();
  void RevealRequested(QWidget* row);
  // Which cards show, or how many rows each sets for the game, changed.
  void CardsChanged();

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
    Card* card = nullptr;              // the card it's in
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
    Card* card = nullptr;
    int at = -1;
  };
  std::string game_id_;
  QWidget* parent_ = nullptr;
  std::map<std::string, Placement> placed_;
  std::map<std::string, Card*> group_cards_;  // by group label, placed or made
  std::function<Card*(const std::string&)> card_maker_;
  std::vector<Card*> hide_when_empty_;
  std::map<std::string, std::string> settings_values_;
  std::string reveal_key_;  // a row RevealKey asked for before the values arrived
  bool values_loaded_ = false;
  bool loading_ = false;
  std::vector<Field> fields_;
};

}  // namespace mira_gui
