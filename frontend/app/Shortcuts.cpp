#include "Shortcuts.h"

#include "KeyBindings.h"
#include "Tray.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QVBoxLayout>

namespace mira_gui::shortcuts {
namespace {

// Spelled out rather than QKeySequence::Quit, which isn't bound on every
// platform Qt supports, and this key shouldn't depend on the platform table.
const QKeySequence kQuit(Qt::CTRL | Qt::Key_Q);

QList<Entry> CommonEntries() {
  return {
      {"Ctrl+Q", "Quit Mira"},
      {"Ctrl+W", "Close this window"},
      {"F1", "Show this list"},
  };
}

void AddRows(QFormLayout* form, QWidget* parent, const QList<Entry>& entries) {
  for (const Entry& entry : entries) {
    auto* keys = new QLabel(entry.keys, parent);
    keys->setProperty("role", "keys");
    form->addRow(keys, new QLabel(entry.description, parent));
  }
}

}  // namespace

Common Install(QMainWindow* window, const QList<Entry>& window_specific) {
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
  common.close_window->setShortcut(keybindings::Register(
      common.close_window, "close_window", "Close this window", QKeySequence::Close));
  QObject::connect(common.close_window, &QAction::triggered, window, [window] { window->close(); });

  common.reference = new QAction("&Keyboard shortcuts", window);
  common.reference->setShortcut(keybindings::Register(
      common.reference, "shortcuts_reference", "Show this list", QKeySequence::HelpContents));
  QObject::connect(common.reference, &QAction::triggered, window,
                   [window, window_specific] { ShowReference(window, window_specific); });

  window->addAction(common.quit);
  window->addAction(common.close_window);
  window->addAction(common.reference);
  return common;
}

void ShowReference(QWidget* parent, const QList<Entry>& window_specific) {
  QDialog dialog(parent);
  dialog.setWindowTitle("Keyboard shortcuts");

  auto* layout = new QVBoxLayout(&dialog);
  auto* form = new QFormLayout();
  form->setHorizontalSpacing(28);

  // This window's own keys first, since that is what the reader came for. The three
  // that work everywhere go last, under a rule.
  AddRows(form, &dialog, window_specific);
  if (!window_specific.isEmpty()) {
    auto* rule = new QFrame(&dialog);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    form->addRow(rule);
  }
  AddRows(form, &dialog, CommonEntries());
  layout->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);

  dialog.exec();
}

}  // namespace mira_gui::shortcuts
