#include "EmulatorPanel.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "../app/Notify.h"
#include "../client/api/Config.h"
#include "../client/api/Emulators.h"
#include "../sources/SourceText.h"
#include "../system/PackageInstall.h"
#include "../widgets/Labels.h"

namespace mira_gui {

EmulatorPanel::EmulatorPanel(const EmulatorInfo& emulator, QWidget* parent) : QWidget(parent), emulator_(emulator) {
  const QString name = QString::fromStdString(emulator.name);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);

  if (!emulator.installed) {
    install_ = new QWidget(this);
    auto* row = new QHBoxLayout(install_);
    row->setContentsMargins(0, 0, 0, 0);
    const QString how = emulator.package.empty() ? "Mira downloads the latest release of " + name + "."
                                                 : name + " comes from your system's packages.";
    row->addWidget(MakeLabel(install_, how, "subtle"), 1);
    auto* choose = new QPushButton("Choose program…", install_);
    choose->setToolTip("Use a copy of " + name + " you already have.");
    connect(choose, &QPushButton::clicked, this, &EmulatorPanel::ChooseProgram);
    row->addWidget(choose);
    auto* install = new QPushButton("Install " + name, install_);
    install->setDefault(true);
    connect(install, &QPushButton::clicked, this, &EmulatorPanel::Install);
    row->addWidget(install);
    layout->addWidget(install_);
  }

  for (const EmulatorFirmware& firmware : emulator.firmware) {
    auto* row = new QHBoxLayout();
    const QString label = QString::fromStdString(firmware.label);
    const QString state = firmware.present ? "Added" : firmware.required ? "Needed" : "Optional";
    row->addWidget(MakeLabel(this, label), 1);
    row->addWidget(MakeLabel(this, state, firmware.present || !firmware.required ? "muted" : "error"));
    auto* add = new QPushButton(firmware.present ? "Replace…" : "Add…", this);
    add->setAccessibleName("Add " + label);
    connect(add, &QPushButton::clicked, this, [this, firmware] { AddFirmware(firmware); });
    row->addWidget(add);
    layout->addLayout(row);
  }
  if (!emulator.firmware.empty()) {
    layout->addWidget(MakeLabel(this, "Mira can't download these: dump them from your own console.", "subtle"));
  }

  error_ = MakeLabel(this, QString(), "error");
  error_->setVisible(false);
  layout->addWidget(error_);
}

void EmulatorPanel::ShowFailure(const QString& what, const ApiError& error) { ShowError(error_, what, error); }

void EmulatorPanel::Install() {
  const QString name = QString::fromStdString(emulator_.name);
  install_->setEnabled(false);
  error_->setVisible(false);
  if (!emulator_.package.empty()) {
    system::EnsurePackages(this, emulator_.package, name, [this](bool ready) {
      install_->setEnabled(true);
      if (ready) emit Changed();
    });
    return;
  }
  ShowLine(error_, "Downloading " + name + "…", "muted");
  api::InstallEmulatorAsync(this, emulator_.id, [this, name](StoreActionResult result) {
    install_->setEnabled(true);
    // Downloaded, but it needs a library from the system's packages too.
    if (!result.ok && result.error.fix.kind == "packages") {
      error_->setVisible(false);
      return system::EnsurePackages(this, emulator_.id, name, [this](bool) { emit Changed(); });
    }
    if (!result.ok) return ShowFailure("Could not install " + name + ".", result.error);
    error_->setVisible(false);
    emit Changed();
  });
}

void EmulatorPanel::ChooseProgram() {
  const QString file = QFileDialog::getOpenFileName(this, "Choose " + QString::fromStdString(emulator_.name));
  if (file.isEmpty()) return;
  const std::string key = "emulators." + emulator_.id + ".bin";
  api::PatchConfigAsync(this, {ConfigEdit{key, "a string", file.toStdString()}}, [this](PatchConfigResult result) {
    if (!result.ok) return ShowFailure("Could not use that program.", result.error);
    emit Changed();
  });
}

void EmulatorPanel::AddFirmware(const EmulatorFirmware& firmware) {
  const QStringList files = QFileDialog::getOpenFileNames(this, "Add " + QString::fromStdString(firmware.label));
  if (files.isEmpty()) return;
  std::vector<std::string> paths;
  for (const QString& file : files) paths.push_back(file.toStdString());
  SendFirmware(firmware.id, paths, false);
}

void EmulatorPanel::SendFirmware(const std::string& firmware_id, const std::vector<std::string>& paths, bool replace) {
  api::AddEmulatorFirmwareAsync(this, emulator_.id, firmware_id, paths, replace,
                                [this, firmware_id, paths](StoreActionResult result) {
    if (!result.ok && result.error.code == "already_there") {
      const QPointer<EmulatorPanel> self(this);  // the panel can be rebuilt while the prompt is up
      if (notify::Confirm(this, "Replace firmware", QString::fromStdString(result.error.message) + ". Replace it?",
                          "Replace", /*destructive=*/true) && self) {
        SendFirmware(firmware_id, paths, true);
      }
      return;
    }
    if (!result.ok) return ShowFailure("Could not add that.", result.error);
    emit Changed();
  });
}

}  // namespace mira_gui
