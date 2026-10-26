#pragma once

#include <QString>
#include <functional>
#include <string>

#include "../client/Types.h"

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QObject;
class QWidget;

namespace mira_gui {

class ListEdit;
class SettingRow;
class Switch;

// One schema setting's row: a switch, enum or runner dropdown, bounded spin
// box, or text field, picked from the entry's shape, under the entry's label.
// Shared by the settings screen, a game's settings and a source page's card.
struct SettingEditor {
  ConfigSchemaEntry entry;
  std::string original;  // last value loaded from the daemon, for change detection
  Switch* toggle = nullptr;
  QLineEdit* line = nullptr;
  QComboBox* combo = nullptr;      // set instead of `line` for a runner key or a schema enum
  QDoubleSpinBox* spin = nullptr;  // set instead of `line` for a bounded number
  ListEdit* list = nullptr;        // set instead of `line` for an array
  SettingRow* row = nullptr;
  // A secret's button that drops the one mirad holds and doesn't send (kSecretSaved). Whether one
  // is held is the line's "secret_saved" property, since editors move around in vectors.
  QAction* clear_secret = nullptr;

  // Builds the row into `parent`. `doc` is the help it shows; empty uses the entry's own.
  SettingRow* Build(QWidget* parent, const QString& doc = {});
  QString Doc() const;         // the entry's doc, noting a per-game override
  QString SearchText() const;  // what the settings search matches on
  QWidget* Input() const;      // the widget to focus
  std::string Text() const;
  void SetText(const std::string& text);
  bool Changed() const { return Text() != original; }
  bool IsDefault() const;
  // Calls `edited` whenever the user changes the value.
  void OnEdited(QObject* context, std::function<void()> edited) const;
};

// Refills a runner combo for `entry`: Auto, then every installed build, or only the builds of its
// runner_kinds; keeps the combo's value.
void FillRunnerCombo(QComboBox* combo, const RunnersResult& runners,
                     const ConfigSchemaEntry& entry);
// An editable runner combo shows a listed runner by its name; anything else is the reference as
// typed.
void ShowRunnerRef(QComboBox* combo, const QString& ref);
QString RunnerRef(const QComboBox* combo);

}  // namespace mira_gui
