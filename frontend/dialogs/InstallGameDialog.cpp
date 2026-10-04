#include "InstallGameDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

#include "../client/MiradClient.h"
#include "../ui/ErrorHelp.h"

namespace mira_gui {

InstallGameDialog::InstallGameDialog(std::string game_id, const std::string& install_path,
                                     const QString& name, QWidget* parent)
    : QDialog(parent), game_id_(std::move(game_id)), install_path_(install_path) {
  setWindowTitle("Install " + name);
  setMinimumWidth(520);

  auto* layout = new QVBoxLayout(this);
  layout->setSpacing(10);

  info_ = new QLabel("Reading the installer…", this);
  info_->setWordWrap(true);
  info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(info_);

  auto* choose_row = new QHBoxLayout();
  auto* choose = new QPushButton("Change…", this);
  choose->setToolTip("Pick a different installer file");
  connect(choose, &QPushButton::clicked, this, [this] {
    const QString picked = QFileDialog::getOpenFileName(
        this, "Installer", QString::fromStdString(install_path_),
        "Installers (*.exe *.msi *.EXE *.MSI);;All files (*)");
    if (!picked.isEmpty()) LoadInfo(picked.toStdString());
  });
  choose_row->addWidget(choose);
  choose_row->addStretch(1);
  layout->addLayout(choose_row);

  error_ = new QLabel(this);
  error_->setProperty("role", "error");
  error_->setWordWrap(true);
  error_->setVisible(false);
  layout->addWidget(error_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  shown_ = buttons->addButton("Show the installer", QDialogButtonBox::AcceptRole);
  shown_->setToolTip("Open the installer's window and click through it yourself");
  quiet_ = buttons->addButton("Install quietly", QDialogButtonBox::AcceptRole);
  quiet_->setToolTip("Run the installer in the background without its window");
  shown_->setEnabled(false);
  quiet_->setVisible(false);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(shown_, &QPushButton::clicked, this, [this] { Install(/*interactive=*/true); });
  connect(quiet_, &QPushButton::clicked, this, [this] { Install(/*interactive=*/false); });
  layout->addWidget(buttons);

  LoadInfo(std::string());
}

void InstallGameDialog::LoadInfo(const std::string& path) {
  shown_->setEnabled(false);
  quiet_->setVisible(false);
  error_->setVisible(false);
  MiradClient::GetInstallerInfoAsync(this, game_id_, path, [this, path](InstallerInfoResult info) {
    if (!info.ok) {
      error_->setText("Could not read the installer: " + error_help::Describe(info.error));
      error_->setVisible(true);
      return;
    }
    installer_ = path;
    const QString file = QFileInfo(QString::fromStdString(info.path)).fileName();
    const QString size = QLocale().formattedDataSize(info.size_bytes);
    QString how;
    if (info.format == "inno") {
      how = "An Inno Setup installer. Mira can run it quietly in the background, or show it.";
    } else if (info.format == "nsis") {
      how = "An NSIS installer. Mira can run it quietly in the background, or show it.";
    } else if (info.format == "msi") {
      how = "A Windows Installer package. Mira can run it quietly in the background, or show it.";
    } else {
      how = "Mira can't run this installer quietly, so its window opens for you to click through.";
    }
    info_->setText(QString("<b>%1</b> (%2)<br>%3").arg(file.toHtmlEscaped(), size, how));
    quiet_->setVisible(info.silent);
    (info.silent ? quiet_ : shown_)->setDefault(true);
    shown_->setEnabled(true);
    adjustSize();  // the text above just grew
  });
}

void InstallGameDialog::Install(bool interactive) {
  shown_->setEnabled(false);
  quiet_->setEnabled(false);
  error_->setVisible(false);
  MiradClient::InstallGameAsync(this, game_id_, interactive, installer_,
                                [this](GameActionResult result) {
                                  if (result.ok) {
                                    accept();
                                    return;
                                  }
                                  shown_->setEnabled(true);
                                  quiet_->setEnabled(true);
                                  error_->setText("Could not start the install: " +
                                                  error_help::Describe(result.error));
                                  error_->setVisible(true);
                                });
}

}  // namespace mira_gui
