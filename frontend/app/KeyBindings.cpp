#include "KeyBindings.h"

#include <QAction>

namespace mira_gui::keybindings {
namespace {

struct Entry {
  QString label;
  QKeySequence default_keys;
  QList<QKeySequence> extra_aliases;
  QList<QPointer<QAction>> actions;
};

QHash<QString, Entry>& Registry() {
  static QHash<QString, Entry> registry;
  return registry;
}

QList<QString>& Order() {
  static QList<QString> order;
  return order;
}

QHash<QString, QKeySequence>& Overrides() {
  static QHash<QString, QKeySequence> overrides;
  return overrides;
}

void Apply(const QString& id, const Entry& entry, const QKeySequence& keys) {
  QList<QKeySequence> sequences{keys};
  sequences.append(entry.extra_aliases);
  for (const QPointer<QAction>& action : entry.actions) {
    if (action) action->setShortcuts(sequences);
  }
}

}  // namespace

QKeySequence Register(QAction* action, const QString& id, const QString& label,
                      const QKeySequence& default_keys, const QList<QKeySequence>& extra_aliases) {
  QHash<QString, Entry>& registry = Registry();
  if (!registry.contains(id)) {
    Order().push_back(id);
    Entry entry;
    entry.label = label;
    entry.default_keys = default_keys;
    entry.extra_aliases = extra_aliases;
    registry.insert(id, entry);
  }
  registry[id].actions.push_back(action);

  const QHash<QString, QKeySequence>& overrides = Overrides();
  return overrides.contains(id) ? overrides.value(id) : registry[id].default_keys;
}

QList<Binding> All() {
  const QHash<QString, Entry>& registry = Registry();
  QList<Binding> bindings;
  for (const QString& id : Order()) {
    const Entry& entry = registry[id];
    bindings.push_back(Binding{id, entry.label, entry.default_keys, entry.extra_aliases});
  }
  return bindings;
}

std::optional<QKeySequence> Override(const QString& id) {
  const QHash<QString, QKeySequence>& overrides = Overrides();
  if (!overrides.contains(id)) return std::nullopt;
  return overrides.value(id);
}

std::map<std::string, std::string> Current() {
  std::map<std::string, std::string> current;
  for (auto it = Overrides().constBegin(); it != Overrides().constEnd(); ++it) {
    current[it.key().toStdString()] = it.value().toString(QKeySequence::PortableText).toStdString();
  }
  return current;
}

void LoadOverrides(const std::map<std::string, std::string>& saved) {
  for (const auto& [id, keys] : saved) {
    SetOverride(QString::fromStdString(id),
               QKeySequence::fromString(QString::fromStdString(keys), QKeySequence::PortableText));
  }
}

void SetOverride(const QString& id, const QKeySequence& keys) {
  Overrides()[id] = keys;
  const QHash<QString, Entry>& registry = Registry();
  if (registry.contains(id)) Apply(id, registry[id], keys);
}

void ResetOverride(const QString& id) {
  QHash<QString, QKeySequence>& overrides = Overrides();
  overrides.remove(id);
  const QHash<QString, Entry>& registry = Registry();
  if (registry.contains(id)) Apply(id, registry[id], registry[id].default_keys);
}

}  // namespace mira_gui::keybindings
