#include "DeleteGameDialog.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace mira_gui {
namespace {

// One destructive option: a checkbox plus the path it would delete.
QCheckBox* AddPathOption(QVBoxLayout* layout, QWidget* parent, const QString& label,
                         const QString& path, const QString& empty_reason) {
  auto* check = new QCheckBox(label, parent);
  layout->addWidget(check);

  auto* path_label = new QLabel(parent);
  path_label->setWordWrap(true);
  path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  path_label->setContentsMargins(22, 0, 0, 6);
  path_label->setProperty("role", "muted");
  layout->addWidget(path_label);

  if (path.isEmpty()) {
    check->setEnabled(false);
    check->setToolTip(empty_reason);
    path_label->setText(empty_reason);
  } else {
    path_label->setText(path);
    path_label->setToolTip(path);
  }
  return check;
}

// The metadata checkbox, destructive warning, and button box, identical
// whether one game or a batch is being asked about.
DeleteChoice FinishDeleteDialog(QDialog& dialog, QVBoxLayout* layout, QCheckBox* files_check,
                                QCheckBox* prefix_check) {
  auto* metadata_check = new QCheckBox("Also delete cached metadata && cover art", &dialog);
  metadata_check->setToolTip(
      "Otherwise the cached data stays on disk, unused.");
  layout->addWidget(metadata_check);

  auto* warning = new QLabel(
      "⚠ Deleting files cannot be undone. Mira refuses to delete any path outside your "
      "library folders and prefix folder.",
      &dialog);
  warning->setWordWrap(true);
  warning->setProperty("role", "error");
  warning->setVisible(false);
  layout->addWidget(warning);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
  QPushButton* remove_button = buttons->addButton("Remove", QDialogButtonBox::AcceptRole);
  // Cancel keeps the focus: this dialog is reached by clicking a Delete
  // button, so Enter should not be a second confirmation of the same intent.
  buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);

  // The button text follows the checkboxes, so the last thing read before
  // clicking says whether this deletes data or only a library entry.
  auto sync_buttons = [&] {
    const bool destructive =
        files_check->isChecked() || prefix_check->isChecked() || metadata_check->isChecked();
    warning->setVisible(destructive);
    remove_button->setText(destructive ? "Delete" : "Remove");
  };
  QObject::connect(files_check, &QCheckBox::toggled, &dialog, sync_buttons);
  QObject::connect(prefix_check, &QCheckBox::toggled, &dialog, sync_buttons);
  QObject::connect(metadata_check, &QCheckBox::toggled, &dialog, sync_buttons);
  // Sized with the warning shown, plus a margin, so ticking a box never
  // clips the wrapped text.
  dialog.setMinimumWidth(460);
  warning->setVisible(true);
  const int height = dialog.sizeHint().height() * 11 / 10;
  sync_buttons();
  dialog.resize(dialog.minimumWidth(), height);

  DeleteChoice choice;
  if (dialog.exec() != QDialog::Accepted) return choice;
  choice.confirmed = true;
  choice.delete_files = files_check->isChecked();
  choice.delete_prefix = prefix_check->isChecked();
  choice.delete_metadata = metadata_check->isChecked();
  return choice;
}

// A Microsoft 365 app is uninstalled by Office's installer; its folder and prefix are shared.
DeleteChoice AskUninstallOfficeApp(QWidget* parent, const QString& name) {
  QMessageBox box(QMessageBox::Question, "Uninstall " + name,
                  QString("Uninstall %1?").arg(name),
                  QMessageBox::Cancel, parent);
  box.setInformativeText("Microsoft 365's installer removes it. Your other apps and your sign-in stay, and it "
                         "can be installed again from Microsoft 365's page.");
  QPushButton* uninstall = box.addButton("Uninstall", QMessageBox::DestructiveRole);
  box.setDefaultButton(QMessageBox::Cancel);
  box.exec();
  DeleteChoice choice;
  if (box.clickedButton() != uninstall) return choice;
  choice.confirmed = true;
  choice.delete_files = true;
  choice.delete_metadata = true;
  return choice;
}

}  // namespace

DeleteChoice AskDeleteGame(QWidget* parent, const QString& name, const QString& install_path,
                           const QString& data_dir, const QString& source) {
  if (source == "office") return AskUninstallOfficeApp(parent, name);
  QDialog dialog(parent);
  dialog.setWindowTitle("Remove game");
  dialog.setModal(true);

  auto* layout = new QVBoxLayout(&dialog);
  layout->setSpacing(8);

  auto* heading = new QLabel(QString("Remove \"%1\" from the library?").arg(name), &dialog);
  heading->setWordWrap(true);
  heading->setProperty("role", "section");
  layout->addWidget(heading);

  auto* explanation = new QLabel(
      "Mira forgets the game. Nothing on disk is touched unless you tick an option below.",
      &dialog);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  // A desktop-entry import only ever linked to another app's own files:
  // Mira never owned install_path/data_dir for it, so offering to delete
  // them would delete someone else's install.
  const bool linked_only = source == "desktop-entry";
  // A store launcher's games and apps share its prefix: it goes when the launcher is removed.
  const bool shared_prefix = source == "battlenet" || source == "ubisoft" || source == "ea" || source == "office";
  QCheckBox* files_check = AddPathOption(
      layout, &dialog, "Also delete the game's files", linked_only ? QString() : install_path,
      linked_only ? "This game links to another app's own files, which Mira doesn't own."
                  : "This game has no install path on record.");
  QCheckBox* prefix_check = AddPathOption(
      layout, &dialog, "Also delete its Wine/Proton prefix", linked_only || shared_prefix ? QString() : data_dir,
      linked_only    ? "This game links to another app's own files, which Mira doesn't own."
      : shared_prefix ? "Its prefix is shared with its launcher and everything else installed through it."
                      : "This game has no prefix (native games don't need one).");

  return FinishDeleteDialog(dialog, layout, files_check, prefix_check);
}

DeleteChoice AskDeleteGames(QWidget* parent, int count) {
  QDialog dialog(parent);
  dialog.setWindowTitle("Remove games");
  dialog.setModal(true);

  auto* layout = new QVBoxLayout(&dialog);
  layout->setSpacing(8);

  auto* heading = new QLabel(QString("Remove %1 games from the library?").arg(count), &dialog);
  heading->setWordWrap(true);
  heading->setProperty("role", "section");
  layout->addWidget(heading);

  auto* explanation = new QLabel(
      "Mira forgets each game. Nothing on disk is touched unless you tick an option below. A "
      "game added via desktop-entry import skips file and prefix deletion automatically, "
      "because Mira never owned those files.",
      &dialog);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  auto* files_check = new QCheckBox("Also delete each game's files", &dialog);
  layout->addWidget(files_check);
  auto* prefix_check = new QCheckBox("Also delete each game's Wine/Proton prefix", &dialog);
  layout->addWidget(prefix_check);

  return FinishDeleteDialog(dialog, layout, files_check, prefix_check);
}

}  // namespace mira_gui
