#include "InstallerCards.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QToolButton>

#include "../client/MiradClient.h"
#include "ArtworkStore.h"
#include "ErrorHelp.h"
#include "Icons.h"

namespace mira_gui {
namespace {

constexpr QSize kCover(40, 60);

// The game's cover before the title, a heading title and a close button: InstallPromptCard's frame.
void BuildHeader(SettingsCard* card, const GameSummary& game, const QString& title, ArtworkStore* artwork,
                 const QString& close_tip, const std::function<void()>& close) {
  auto* cover = new QLabel(card);
  cover->setFixedSize(kCover);
  const auto show_cover = [card, cover, artwork, game] {
    cover->setPixmap(artwork->Cover(game, kCover, card->devicePixelRatioF()));
  };
  show_cover();
  QObject::connect(artwork, &ArtworkStore::CoverChanged, cover,
                   [show_cover, id = QString::fromStdString(game.id)](const QString& changed) {
                     if (changed == id) show_cover();
                   });
  card->SetLeading(cover);

  auto* heading = new QLabel(title, card);
  heading->setProperty("role", "heading");
  heading->setWordWrap(true);
  card->SetTitleWidget(heading);

  auto* button = new QToolButton(card);
  button->setAutoRaise(true);
  button->setIcon(icons::For(icons::Glyph::Close));
  button->setToolTip(close_tip);
  QObject::connect(button, &QToolButton::clicked, card, close);
  card->Header()->addWidget(button, 0, Qt::AlignTop);
}

QLabel* ErrorLine(SettingsCard* card) {
  auto* error = new QLabel(card);
  error->setProperty("role", "error");
  error->setWordWrap(true);
  error->setContentsMargins(18, 4, 18, 0);
  error->hide();
  card->AddRow(error);
  return error;
}

QHBoxLayout* ButtonRow(SettingsCard* card) {
  auto* buttons = new QWidget(card);
  auto* layout = new QHBoxLayout(buttons);
  layout->setContentsMargins(18, 12, 18, 10);
  layout->setSpacing(8);
  layout->addStretch(1);
  card->AddRow(buttons);
  return layout;
}

QString FormatName(const std::string& format) {
  if (format == "inno") return "Inno Setup";
  if (format == "nsis") return "NSIS";
  if (format == "msi") return "Windows Installer";
  return {};
}

}  // namespace

InstallerCard::InstallerCard(const GameSummary& game, ArtworkStore* artwork, QWidget* parent)
    : SettingsCard(QString(), parent), game_id_(game.id), install_path_(game.install_path) {
  setFixedWidth(560);
  BuildHeader(this, game, "Install " + QString::fromStdString(game.name) + "?", artwork, "Not now",
              [this] { emit CloseRequested(); });

  auto* installer = new SettingRow("Installer", QString(), this);
  file_ = new QLabel("Reading the installer…", installer);
  file_->setWordWrap(true);
  file_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  installer->SetBelow(file_);
  auto* change = new QPushButton("Change…", installer);
  change->setToolTip("Pick a different installer file");
  connect(change, &QPushButton::clicked, this, [this] {
    const QString picked = QFileDialog::getOpenFileName(this, "Installer", QString::fromStdString(install_path_),
                                                        "Installers (*.exe *.msi *.EXE *.MSI);;All files (*)");
    if (!picked.isEmpty()) LoadInfo(picked.toStdString());
  });
  installer->AddControl(change);
  AddRow(installer);
  error_ = ErrorLine(this);

  QHBoxLayout* buttons = ButtonRow(this);
  auto* later = new QPushButton("Not now", this);
  connect(later, &QPushButton::clicked, this, &InstallerCard::CloseRequested);
  buttons->addWidget(later);
  quiet_ = new QPushButton("Install quietly", this);
  quiet_->setToolTip("Run the installer in the background without its window");
  // Hidden until the format allows it, keeping its place so the row doesn't shift.
  QSizePolicy keep = quiet_->sizePolicy();
  keep.setRetainSizeWhenHidden(true);
  quiet_->setSizePolicy(keep);
  quiet_->hide();
  connect(quiet_, &QPushButton::clicked, this, [this] { Install(/*interactive=*/false); });
  buttons->addWidget(quiet_);
  shown_ = new QPushButton("Show the installer", this);
  shown_->setToolTip("Open the installer's window and click through it yourself");
  shown_->setEnabled(false);
  connect(shown_, &QPushButton::clicked, this, [this] { Install(/*interactive=*/true); });
  buttons->addWidget(shown_);

  LoadInfo(std::string());
}

void InstallerCard::LoadInfo(const std::string& path) {
  shown_->setEnabled(false);
  quiet_->hide();
  error_->hide();
  MiradClient::GetInstallerInfoAsync(this, game_id_, path, [this, path](InstallerInfoResult info) {
    if (!info.ok) {
      file_->setText(QString());
      error_->setText("Could not read the installer: " + error_help::Describe(info.error));
      error_->show();
      return;
    }
    installer_ = path;
    QStringList parts{QFileInfo(QString::fromStdString(info.path)).fileName(),
                      QLocale().formattedDataSize(info.size_bytes)};
    if (const QString format = FormatName(info.format); !format.isEmpty()) parts << format;
    file_->setText(parts.join("  ·  "));
    file_->setToolTip(QString::fromStdString(info.path));
    quiet_->setVisible(info.silent);
    (info.silent ? quiet_ : shown_)->setDefault(true);
    shown_->setEnabled(true);
  });
}

void InstallerCard::Install(bool interactive) {
  shown_->setEnabled(false);
  quiet_->setEnabled(false);
  error_->hide();
  MiradClient::InstallGameAsync(this, game_id_, interactive, installer_, [this](GameActionResult result) {
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

InstallerLeftoverCard::InstallerLeftoverCard(const GameSummary& game, const std::string& installer_dir,
                                             std::int64_t bytes, ArtworkStore* artwork, QWidget* parent)
    : SettingsCard(QString(), parent), game_id_(game.id) {
  setFixedWidth(560);
  BuildHeader(this, game, "Delete " + QString::fromStdString(game.name) + "'s installer?", artwork, "Keep it",
              [this] { emit CloseRequested(); });

  auto* folder = new SettingRow("Installer folder", QString(), this);
  auto* path = new QLabel(QString("%1  ·  %2").arg(QString::fromStdString(installer_dir),
                                                  QLocale().formattedDataSize(bytes)),
                          folder);
  path->setWordWrap(true);
  path->setTextInteractionFlags(Qt::TextSelectableByMouse);
  folder->SetBelow(path);
  AddRow(folder);
  error_ = ErrorLine(this);

  QHBoxLayout* buttons = ButtonRow(this);
  auto* keep = new QPushButton("Keep", this);
  keep->setDefault(true);  // a stray Return never deletes
  connect(keep, &QPushButton::clicked, this, &InstallerLeftoverCard::CloseRequested);
  buttons->addWidget(keep);
  delete_ = new QPushButton("Delete", this);
  connect(delete_, &QPushButton::clicked, this, [this] {
    delete_->setEnabled(false);
    error_->hide();
    MiradClient::DeleteInstallerAsync(this, game_id_, [this](GameActionResult result) {
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

}  // namespace mira_gui
