#include "InstallerCards.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QToolButton>
#include <algorithm>

#include "../app/ErrorHelp.h"
#include "../client/api/Games.h"
#include "../library/ArtworkStore.h"
#include "../theme/Icons.h"
#include "../widgets/FlatCombo.h"
#include "../widgets/Labels.h"
#include "CardParts.h"
#include "LaunchOptionsEdit.h"

namespace mira_gui {

InstallerCard::InstallerCard(const GameSummary& game, const InstallerInfoResult& info,
                             ArtworkStore* artwork, QWidget* parent)
    : SettingsCard(QString(), parent), game_id_(game.id), install_path_(game.install_path) {
  setFixedWidth(560);
  AddGameCardHeading(this, game, "Install " + QString::fromStdString(game.name) + "?", artwork,
                     "Not now", [this] { emit CloseRequested(); });

  auto* installer = new SettingRow("Installer", QString(), this);
  file_ = new QLabel(installer);
  file_->setWordWrap(true);
  file_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  installer->SetBelow(file_);
  auto* change = new QPushButton("Change…", installer);
  change->setToolTip("Pick a different installer file");
  connect(change, &QPushButton::clicked, this, [this] {
    const QString picked =
        QFileDialog::getOpenFileName(this, "Installer", QString::fromStdString(install_path_),
                                     "Installers (*.exe *.msi *.EXE *.MSI);;All files (*)");
    if (!picked.isEmpty()) LoadInfo(picked.toStdString());
  });
  installer->AddControl(change);
  AddRow(installer);
  // A disc or mounted image is full or read-only: the game's prefix goes on a library root's disk.
  if (!info.targets.empty()) {
    auto* row = new SettingRow("Install onto", QString(), this);
    target_ = new FlatCombo(row);
    for (const InstallerInfoResult::Target& target : info.targets) {
      target_->addItem(
          QString("%1  ·  %2 free")
              .arg(HomeRelative(QString::fromStdString(target.path)), SizeText(target.free_bytes)),
          QString::fromStdString(target.path));
    }
    row->AddControl(target_);
    AddRow(row);
  }
  error_ = AddErrorLine(this);

  QHBoxLayout* buttons = AddButtonRow(this);
  auto* later = new QPushButton("Not now", this);
  connect(later, &QPushButton::clicked, this, &InstallerCard::CloseRequested);
  buttons->addWidget(later);
  quiet_ = new QPushButton("Install quietly", this);
  quiet_->setToolTip("Run the installer in the background without its window");
  // Hidden until the format allows it, keeping its place so the row doesn't shift.
  KeepSpaceWhenHidden(quiet_);
  quiet_->hide();
  connect(quiet_, &QPushButton::clicked, this, [this] { Install(/*interactive=*/false); });
  buttons->addWidget(quiet_);
  shown_ = new QPushButton("Show the installer", this);
  shown_->setToolTip("Open the installer's window and click through it yourself");
  shown_->setEnabled(false);
  connect(shown_, &QPushButton::clicked, this, [this] { Install(/*interactive=*/true); });
  buttons->addWidget(shown_);

  ShowInfo(info, std::string());
}

void InstallerCard::LoadInfo(const std::string& path) {
  shown_->setEnabled(false);
  quiet_->hide();
  error_->hide();
  const int request = ++info_request_;
  api::GetInstallerInfoAsync(this, game_id_, path, [this, path, request](InstallerInfoResult info) {
    if (request == info_request_) ShowInfo(info, path);
  });
}

void InstallerCard::ShowInfo(const InstallerInfoResult& info, const std::string& path) {
  if (!info.ok) {
    // The last pick that loaded stays, if any did.
    const bool loaded = !file_->text().isEmpty();
    shown_->setEnabled(loaded);
    quiet_->setVisible(loaded && silent_);
    error_->setText("Could not read the installer: " + error_help::Describe(info.error));
    error_->show();
    return;
  }
  installer_ = path;
  QStringList parts{QFileInfo(QString::fromStdString(info.path)).fileName(),
                    SizeText(info.size_bytes)};
  if (info.estimated_install_bytes > 0)
    parts << "about " + SizeText(info.estimated_install_bytes) + " installed";
  if (const QString format = InstallerFormatName(info.format); !format.isEmpty()) parts << format;
  file_->setText(parts.join("  ·  "));
  file_->setToolTip(QString::fromStdString(info.path));
  silent_ = info.silent;
  quiet_->setVisible(info.silent);
  (info.silent ? quiet_ : shown_)->setDefault(true);
  shown_->setEnabled(true);
}

void InstallerCard::Install(bool interactive) {
  shown_->setEnabled(false);
  quiet_->setEnabled(false);
  error_->hide();
  const std::string target =
      target_ != nullptr ? target_->currentData().toString().toStdString() : std::string();
  api::InstallGameAsync(
      this, game_id_, interactive, installer_, target, [this](GameActionResult result) {
        if (result.ok) {
          emit Started();
          return;
        }
        shown_->setEnabled(true);
        quiet_->setEnabled(true);
        error_->setText("Could not start the install: " + error_help::Describe(result.error));
        error_->show();
      });
}

InstallerLeftoverCard::InstallerLeftoverCard(const GameSummary& game,
                                             const std::string& installer_dir, std::int64_t bytes,
                                             ArtworkStore* artwork, QWidget* parent)
    : SettingsCard(QString(), parent), game_id_(game.id) {
  setFixedWidth(560);
  AddGameCardHeading(this, game, "Delete " + QString::fromStdString(game.name) + "'s installer?",
                     artwork, "Keep it", [this] { emit CloseRequested(); });

  auto* folder = new SettingRow("Installer folder", QString(), this);
  auto* path = new QLabel(
      QString("%1  ·  %2").arg(QString::fromStdString(installer_dir), SizeText(bytes)), folder);
  path->setWordWrap(true);
  path->setTextInteractionFlags(Qt::TextSelectableByMouse);
  folder->SetBelow(path);
  AddRow(folder);
  error_ = AddErrorLine(this);

  QHBoxLayout* buttons = AddButtonRow(this);
  auto* keep = new QPushButton("Keep", this);
  keep->setDefault(true);  // a stray Return never deletes
  connect(keep, &QPushButton::clicked, this, &InstallerLeftoverCard::CloseRequested);
  buttons->addWidget(keep);
  delete_ = new QPushButton("Delete", this);
  connect(delete_, &QPushButton::clicked, this, [this] {
    delete_->setEnabled(false);
    error_->hide();
    api::DeleteInstallerAsync(this, game_id_, [this](GameActionResult result) {
      if (result.ok) {
        emit CloseRequested();
        return;
      }
      delete_->setEnabled(true);
      error_->setText("Could not delete it: " + error_help::Describe(result.error));
      error_->show();
    });
  });
  buttons->addWidget(delete_);
}

ProgramsFoundCard::ProgramsFoundCard(const GameSummary& game, const ProgramsFoundEvent& event,
                                     ArtworkStore* artwork, std::function<GameLaunch()> current,
                                     QWidget* parent)
    : SettingsCard(QString(), parent), game_id_(game.id), current_(std::move(current)) {
  setFixedWidth(560);
  AddGameCardHeading(this, game,
                     QString::fromStdString(event.by) + " installed " +
                         (event.programs.size() == 1 ? "a program" : "programs"),
                     artwork, "Close", [this] { emit CloseRequested(); });
  for (const ProgramsFoundEvent::Program& program : event.programs) {
    auto* row = new SettingRow(QString::fromStdString(program.name), QString(), this);
    auto* path = new QLabel(QString::fromStdString(program.exe), row);
    path->setWordWrap(true);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setProperty("role", "subtle");
    row->SetBelow(path);
    auto* mode = new FlatCombo(row);
    for (const auto& [id, text] : kLaunchModes) mode->addItem(text, QString(id));
    mode->setCurrentIndex(mode->findData(QString("alongside")));
    row->AddControl(mode);
    auto* add = new QPushButton("Add", row);
    add->setToolTip("Add it to " + QString::fromStdString(game.name) + "'s launch options");
    connect(add, &QPushButton::clicked, this, [this, program, mode, add] {
      // One at a time, since each saves the whole set.
      for (QPushButton* button : adds_) button->setEnabled(false);
      mode->setEnabled(false);
      error_->hide();
      // The game's options as they are now, with ones added here that it may not show yet.
      GameLaunch launch = current_();
      for (const LaunchOption& added : added_) {
        if (std::ranges::none_of(launch.options, [&](const LaunchOption& option) {
              return option.exe == added.exe;
            }))
          launch.options.push_back(added);
      }
      // mirad makes its id from the name.
      const LaunchOption option{.name = program.name,
                                .mode = mode->currentData().toString().toStdString(),
                                .exe = program.exe};
      launch.options.push_back(option);
      GamePatch patch;
      patch.launch = launch;
      api::PatchGameAsync(this, game_id_, patch, [this, option, mode, add](PatchGameResult result) {
        if (result.ok) {
          added_.push_back(option);
          add->setText("Added");
        } else {
          mode->setEnabled(true);
          error_->setText("Could not add it: " + error_help::Describe(result.error));
          error_->show();
        }
        for (QPushButton* button : adds_) button->setEnabled(button->text() != "Added");
      });
    });
    adds_.push_back(add);
    row->AddControl(add);
    AddRow(row);
  }
  error_ = AddErrorLine(this);

  QHBoxLayout* buttons = AddButtonRow(this);
  auto* done = new QPushButton("Done", this);
  done->setDefault(true);
  connect(done, &QPushButton::clicked, this, &ProgramsFoundCard::CloseRequested);
  buttons->addWidget(done);
}

}  // namespace mira_gui
