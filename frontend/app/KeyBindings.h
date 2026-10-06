#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QString>

#include <map>
#include <optional>
#include <string>

class QAction;

namespace mira_gui::keybindings {

// A registry of the app's editable keyboard shortcuts, shared across every
// open window (one registry, not one per window, so Ctrl+Q means the same
// thing everywhere). Register() and the saved override can arrive in
// either order (a second window opened after prefs load, or before), and
// SetOverride() re-applying live to every registered QAction is what keeps
// both orderings correct.

struct Binding {
  QString id;
  QString label;
  QKeySequence default_keys;
  QList<QKeySequence> extra_aliases;  // fixed, always-on, not user-editable
};

// Registers `action` under `id` (stable, lower_snake_case, not the label)
// and returns the QKeySequence it should use right now: the user's own
// override if set, else `default_keys`. Caller still builds and installs
// the full QList<QKeySequence> (this plus `extra_aliases`) itself.
QKeySequence Register(QAction* action, const QString& id, const QString& label,
                      const QKeySequence& default_keys, const QList<QKeySequence>& extra_aliases = {});

// Every distinct id registered so far, in registration order, which is what the
// Settings screen's Shortcuts category lists.
QList<Binding> All();

// This id's current override, if SetOverride (directly, or via
// LoadOverrides) was ever called for it and it hasn't been reset since. An
// override can be an empty QKeySequence: "no shortcut" is a valid, deliberate
// choice, not the same state as "never overridden".
std::optional<QKeySequence> Override(const QString& id);

// Every current override, id -> QKeySequence::toString() (PortableText).
// what FrontendPrefs.shortcut_overrides round-trips.
std::map<std::string, std::string> Current();

// Makes a saved FrontendPrefs.shortcut_overrides map the whole set of
// overrides: ids missing from it go back to their defaults. See the class
// comment for why this composes correctly regardless of whether it runs
// before or after the ids in question are Register()ed.
void LoadOverrides(const std::map<std::string, std::string>& saved);

// Sets (or, with an empty QKeySequence, deliberately clears) `id`'s override
// and re-applies it (this key plus whatever extra_aliases that id was
// registered with) to every currently-live QAction registered under it.
void SetOverride(const QString& id, const QKeySequence& keys);

// Back to whatever default_keys this id's Register() calls used.
void ResetOverride(const QString& id);

}  // namespace mira_gui::keybindings
