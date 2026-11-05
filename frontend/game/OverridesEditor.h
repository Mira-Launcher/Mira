#pragma once

#include <QWidget>
#include <algorithm>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../settings/SettingEditor.h"
#include "../theme/Icons.h"

class QLabel;
class QToolButton;

namespace mira_gui {

class SettingsCard;
class SettingsNavWidget;
class SettingsPage;

// The "just for this game" half of a game's settings: every overridable
// setting, resolved through default -> settings.toml -> this game, editable
// the same way the settings screen edits the global value, each row's clear
// button dropping back to the layer underneath.
//
// Its own widget because it talks to a different endpoint than the dialog
// around it: the dialog saves fields via PATCH .../games/{id}, this saves
// keys via PATCH .../games/{id}/config. The dialog only asks it what changed.
class OverridesEditor : public QWidget {
  Q_OBJECT

 public:
  explicit OverridesEditor(std::string game_id, QWidget* parent = nullptr);

  // Fetches the schema, builds a row per key, then fills in this game's
  // resolved values. Non-fatal if either request fails: the rest of the
  // dialog still works without this section.
  void Load();

  // The keys whose value the user actually changed, ready for
  // PATCH /v1/games/{id}/config. Empty when nothing was touched, so a dialog
  // opened and saved unedited sends no override patch at all.
  std::vector<GameConfigEdit> PendingEdits() const;

  // Call after PendingEdits() was successfully applied. It resets the
  // change-tracking baseline without a re-fetch, so PendingEdits() goes
  // back to empty.
  void MarkSaved();
  // Puts every edited row back to its loaded value.
  void DiscardChanges();

  // A page of the host's own, listed before the schema's categories.
  SettingsPage* AddPage(const QString& title, icons::Glyph glyph);
  // Builds `keys`' rows into the host's `card` instead of these pages. Call before Load.
  void HostRows(std::vector<std::string> keys, SettingsCard* card);
  bool Hosts(const std::string& key) const { return std::ranges::contains(host_keys_, key); }
  // Re-splits the pages after the host's cards changed, and starts at the top.
  void ShowFromTop();
  // Scrolls `key`'s row into view, once the values have arrived.
  void RevealKey(const std::string& key);

 signals:
  void Changed();

 private:
  // One row: an overridable schema key with the same editor the settings
  // screen gives it and the layer the shown value came from.
  struct Field : SettingEditor {
    std::string layer;  // "default" | "config" | "game"
    QLabel* layer_label = nullptr;
    QToolButton* clear = nullptr;  // drops this game's override; shown while the layer is "game"
  };

  void BuildRows(const ConfigSchemaResult& schema);
  void ApplyValues(const GameConfigResult& config);
  void Reload();
  void ResetField(size_t index);
  Field* FindField(const std::string& key);

  std::string game_id_;
  // The overrides a reset is reloading for; their edits are dropped.
  std::vector<std::string> resetting_keys_;
  std::string reveal_key_;  // a row RevealKey asked for before the values arrived
  bool values_loaded_ = false;
  SettingsNavWidget* nav_ = nullptr;
  std::vector<Field> fields_;
  std::vector<std::string> host_keys_;
  SettingsCard* host_card_ = nullptr;
};

}  // namespace mira_gui
