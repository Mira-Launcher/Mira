#include "AddManualGameDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <filesystem>

#include "../client/MiradClient.h"
#include "../app/Notify.h"

namespace mira_gui {

AddManualGameDialog::AddManualGameDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Add game manually");
  setMinimumWidth(680);

  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(8);

  auto* explanation = new QLabel(
      "For an installer or a folder outside every configured library root. Everything else "
      "(cover art, ProtonDB rating) fills in the same way a scanned game's does.",
      this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);

  auto* form = new QFormLayout();

  install_path_ = new QLineEdit(this);
  install_path_->setPlaceholderText("The game's own folder");
  auto* browse_install = new QPushButton("Browse…", this);
  connect(browse_install, &QPushButton::clicked, this, [this] {
    const QString selected = QFileDialog::getExistingDirectory(this, "Select the game's folder");
    if (!selected.isEmpty()) install_path_->setText(selected);
  });
  auto* install_row_widget = new QWidget(this);
  auto* install_row = new QHBoxLayout(install_row_widget);
  install_row->setContentsMargins(0, 0, 0, 0);
  install_row->setSpacing(8);
  install_row->addWidget(install_path_, /*stretch=*/1);
  install_row->addWidget(browse_install);
  form->addRow("Install path", install_row_widget);

  exe_path_ = new QLineEdit(this);
  exe_path_->setPlaceholderText("Relative to the install path");
  connect(exe_path_, &QLineEdit::textChanged, this, [this](const QString& text) {
    add_->setEnabled(!text.trimmed().isEmpty() && !install_path_->text().trimmed().isEmpty());
  });
  connect(install_path_, &QLineEdit::textChanged, this, [this] {
    add_->setEnabled(!exe_path_->text().trimmed().isEmpty() &&
                     !install_path_->text().trimmed().isEmpty());
  });
  auto* browse_exe = new QPushButton("Browse…", this);
  connect(browse_exe, &QPushButton::clicked, this, &AddManualGameDialog::BrowseExe);
  auto* exe_row_widget = new QWidget(this);
  auto* exe_row = new QHBoxLayout(exe_row_widget);
  exe_row->setContentsMargins(0, 0, 0, 0);
  exe_row->setSpacing(8);
  exe_row->addWidget(exe_path_, /*stretch=*/1);
  exe_row->addWidget(browse_exe);
  form->addRow("Executable", exe_row_widget);

  name_ = new QLineEdit(this);
  name_->setPlaceholderText("Defaults to the install folder's name");
  form->addRow("Name", name_);

  platform_ = new QComboBox(this);
  platform_->addItem("Auto-detect from the executable", QString());
  platform_->addItem("Windows", "windows");
  platform_->addItem("Native (Linux)", "native");
  form->addRow("Platform", platform_);

  is_installer_ = new QCheckBox("This is an installer, not the game itself", this);
  is_installer_->setToolTip(
      "Adds it as needing install instead of ready. Run the installer, then mark the game as "
      "installed once it is in place.");
  form->addRow(QString(), is_installer_);

  layout->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  add_ = buttons->addButton("Add", QDialogButtonBox::AcceptRole);
  add_->setEnabled(false);
  connect(add_, &QPushButton::clicked, this, &AddManualGameDialog::Submit);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

void AddManualGameDialog::Prefill(const QString& install_path, const QString& name) {
  install_path_->setText(install_path);
  name_->setText(name);
  exe_path_->setFocus();
}

void AddManualGameDialog::BrowseExe() {
  const QString start =
      install_path_->text().trimmed().isEmpty() ? QString() : install_path_->text();
  const QString selected = QFileDialog::getOpenFileName(this, "Select the executable", start);
  if (selected.isEmpty()) return;

  // Picking the exe before the install path says which folder is the game's
  // own, so filling install_path_ from it beats making the user go browse a
  // second time for what BrowseExe just showed them.
  if (install_path_->text().trimmed().isEmpty()) {
    const QFileInfo info(selected);
    install_path_->setText(info.absolutePath());
    exe_path_->setText(info.fileName());
    return;
  }

  std::error_code ec;
  const std::filesystem::path relative =
      std::filesystem::relative(selected.toStdString(), install_path_->text().toStdString(), ec);
  exe_path_->setText(!ec && !relative.empty() ? QString::fromStdString(relative.string())
                                              : selected);
}

void AddManualGameDialog::Submit() {
  const std::string install_path = install_path_->text().trimmed().toStdString();
  const std::string exe_path = exe_path_->text().trimmed().toStdString();
  if (install_path.empty() || exe_path.empty()) return;  // Add is disabled for this case

  setEnabled(false);
  add_->setText("Adding…");
  MiradClient::AddManualGameAsync(
      this, install_path, exe_path, name_->text().trimmed().toStdString(),
      platform_->currentData().toString().toStdString(), is_installer_->isChecked(),
      [this](GameDetailResult result) {
        setEnabled(true);
        add_->setText("Add");
        if (!result.ok) {
          notify::FailedRequest(this, "Could not add that game.", result.error);
          return;
        }
        accept();
      });
}

}  // namespace mira_gui
