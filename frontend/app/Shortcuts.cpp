#include "Shortcuts.h"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>

#include "../game/CardParts.h"
#include "../settings/SettingsCard.h"
#include "../widgets/WindowCards.h"
#include "KeyBindings.h"
#include "Tray.h"

namespace mira_gui::shortcuts {
namespace {

// Spelled out rather than QKeySequence::Quit, which isn't bound on every
// platform Qt supports, and this key shouldn't depend on the platform table.
const QKeySequence kQuit(Qt::CTRL | Qt::Key_Q);

constexpr char kEverywhere[] = "Everywhere";

QList<Entry> CommonEntries() {
  return {
      {"Ctrl+Q", "Quit Mira", "quit", kEverywhere},
      {"Ctrl+W", "Close this window", "close_window", kEverywhere},
      {"F1", "Show this list", "shortcuts_reference", kEverywhere},
  };
}

// "Ctrl+Shift+A, F5" as keycaps; a lone "+" (as in "Ctrl++") is a key of its own.
QWidget* Keycaps(const QString& text, QWidget* parent) {
  auto* caps = new QWidget(parent);
  auto* layout = new QHBoxLayout(caps);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);
  const QStringList combos = text.split(", ");
  for (int c = 0; c < combos.size(); ++c) {
    if (c > 0) {
      auto* gap = new QLabel("or", caps);
      gap->setProperty("role", "muted");
      layout->addWidget(gap);
    }
    const QString& combo = combos[c];
    QStringList keys;
    QString key;
    for (const QChar ch : combo) {
      if (ch == '+' && !key.isEmpty()) {
        keys << key;
        key.clear();
      } else {
        key += ch;
      }
    }
    if (!key.isEmpty()) keys << key;
    for (const QString& name : keys) {
      auto* cap = new QLabel(name, caps);
      cap->setProperty("role", "keycap");
      layout->addWidget(cap);
    }
  }
  return caps;
}

// Every key a window answers to, its groups in two columns, each group's rows in order.
class ReferenceCard : public SettingsCard {
 public:
  ReferenceCard(const QList<Entry>& entries, const std::function<void()>& change)
      : SettingsCard(QString()) {
    SetProminentTitle();
    AddCardHeading(this, "Keyboard shortcuts", "Close", [this] { cards::Close(this); });
    if (change) {
      auto* edit = new QPushButton("Change…", this);
      edit->setFlat(true);
      edit->setToolTip("Settings › Shortcuts");
      connect(edit, &QPushButton::clicked, this, [this, change] {
        cards::Close(this);
        change();
      });
      Header()->insertWidget(Header()->count() - 1, edit, 0, Qt::AlignVCenter);
    }

    std::vector<std::pair<QString, QList<Entry>>> groups;
    for (const Entry& entry : entries) {
      const QString text =
          entry.id.isEmpty() ? entry.keys : keybindings::KeysText(entry.id, entry.keys);
      if (text.isEmpty()) continue;  // unbound
      Entry shown = entry;
      shown.keys = text;
      auto it = std::ranges::find(groups, entry.group, &std::pair<QString, QList<Entry>>::first);
      if (it == groups.end()) it = groups.insert(groups.end(), {entry.group, {}});
      it->second << shown;
    }

    auto* body = new QWidget(this);
    auto* columns = new QHBoxLayout(body);
    columns->setContentsMargins(18, 4, 18, 12);
    columns->setSpacing(28);
    QVBoxLayout* column[2] = {new QVBoxLayout(), new QVBoxLayout()};
    for (QVBoxLayout* layout : column) layout->setSpacing(0);
    int rows[2] = {0, 0};
    for (const auto& [group, list] : groups) {
      // Into the shorter column, so the two come out about as long.
      const int at = rows[1] < rows[0] ? 1 : 0;
      rows[at] += static_cast<int>(list.size()) + 1;
      auto* heading = new QLabel(group.toUpper(), body);
      heading->setProperty("role", "group_heading");
      heading->setContentsMargins(0, 10, 0, 4);
      column[at]->addWidget(heading);
      for (const Entry& entry : list) {
        auto* line = new QHBoxLayout();
        line->setContentsMargins(0, 5, 0, 5);
        auto* what = new QLabel(entry.description, body);
        line->addWidget(what, 1);
        line->addSpacing(16);
        line->addWidget(Keycaps(entry.keys, body), 0, Qt::AlignVCenter);
        column[at]->addLayout(line);
      }
    }
    for (QVBoxLayout* layout : column) {
      layout->addStretch(1);
      columns->addLayout(layout, 1);
    }
    AddRow(body);
    // As wide as the longest line, so no description wraps.
    setFixedWidth(sizeHint().width());
  }
};

}  // namespace

Common Install(QMainWindow* window, const QList<Entry>& window_specific,
               std::function<void()> open_settings) {
  Common common;

  common.quit = new QAction("&Quit", window);
  common.quit->setShortcut(keybindings::Register(common.quit, "quit", "Quit Mira", kQuit));
  common.quit->setMenuRole(QAction::QuitRole);
  // tray::RequestQuit(), not closeAllWindows() directly: with a tray icon
  // attached, a window's closeEvent hides it instead of closing it unless
  // it already knows this is a real quit. Every window still gets its
  // closeEvent either way, so LibraryWindow's saved layout isn't skipped.
  QObject::connect(common.quit, &QAction::triggered, window, [] { tray::RequestQuit(); });

  common.close_window = new QAction("&Close window", window);
  common.close_window->setShortcut(keybindings::Register(common.close_window, "close_window",
                                                         "Close this window", QKeySequence::Close));
  QObject::connect(common.close_window, &QAction::triggered, window, [window] { window->close(); });

  common.reference = new QAction("&Keyboard shortcuts", window);
  common.reference->setShortcut(keybindings::Register(
      common.reference, "shortcuts_reference", "Show this list", QKeySequence::HelpContents));
  QObject::connect(common.reference, &QAction::triggered, window,
                   [window, window_specific, open_settings] {
                     ShowReference(window, window_specific, open_settings);
                   });

  window->addAction(common.quit);
  window->addAction(common.close_window);
  window->addAction(common.reference);
  return common;
}

void ShowReference(QWidget* parent, const QList<Entry>& window_specific,
                   const std::function<void()>& open_settings) {
  // This window's own keys first, since that is what the reader came for.
  cards::Show(parent, new ReferenceCard(window_specific + CommonEntries(), open_settings));
}

QString HiddenGamesHint() {
  const QString keys = keybindings::KeysText("toggle_hidden", "Ctrl+H");
  return "(" + (keys.isEmpty() ? QString("the Hidden filter") : keys) + " shows hidden games)";
}

}  // namespace mira_gui::shortcuts
