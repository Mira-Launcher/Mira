#include "InstallPromptCard.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <filesystem>

#include "../client/Types.h"
#include "../app/Notify.h"
#include "../library/ArtworkStore.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui {
namespace {

constexpr QSize kCover(40, 60);

// The path as Windows programs see it: what follows the prefix's drive_c.
QString WindowsPath(const std::string& path) {
  const QString text = QString::fromStdString(path);
  const int at = text.indexOf("/drive_c/");
  return at < 0 ? text : "C:" + text.mid(at + 8).replace('/', '\\');
}

}  // namespace

InstallPromptCard::InstallPromptCard(const GameSummary& game, const std::string& install_path,
                                     const std::string& exe_path, ArtworkStore* artwork, QWidget* parent)
    : SettingsCard(QString(), parent), install_path_(install_path), exe_path_(exe_path) {
  setFixedWidth(560);

  auto* cover = new QLabel(this);
  cover->setFixedSize(kCover);
  const auto show_cover = [this, cover, artwork, game] {
    cover->setPixmap(artwork->Cover(game, kCover, devicePixelRatioF()));
  };
  show_cover();
  connect(artwork, &ArtworkStore::CoverChanged, cover, [show_cover, id = QString::fromStdString(game.id)](const QString& changed) {
    if (changed == id) show_cover();
  });
  SetLeading(cover);

  auto* heading = new QWidget(this);
  auto* heading_layout = new QVBoxLayout(heading);
  heading_layout->setContentsMargins(0, 0, 0, 0);
  heading_layout->setSpacing(2);
  auto* title = new QLabel(QString::fromStdString(game.name) + " installed a program", heading);
  title->setProperty("role", "heading");
  heading_layout->addWidget(title);
  auto* question = new QLabel("It ran as an installer. Use the program it installed instead?", heading);
  question->setProperty("role", "subtle");
  question->setWordWrap(true);
  heading_layout->addWidget(question);
  SetTitleWidget(heading);

  auto* close = new QToolButton(this);
  close->setAutoRaise(true);
  close->setIcon(icons::For(icons::Glyph::Close));
  close->setToolTip("Keep as is");
  connect(close, &QToolButton::clicked, this, &InstallPromptCard::CloseRequested);
  Header()->addWidget(close, 0, Qt::AlignTop);

  // The path under its label, full width, so the file name shows.
  auto* program = new SettingRow("Program", QString(), this);
  program_ = new QLabel(program);
  program_->setWordWrap(true);
  program_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  program->SetBelow(program_);
  auto* choose = new QPushButton("Choose…", program);
  connect(choose, &QPushButton::clicked, this, &InstallPromptCard::Choose);
  program->AddControl(choose);
  AddRow(program);

  auto* app_row = new SettingRow("This is an app, not a game", QString(), this);
  app_ = new Switch(app_row);
  app_row->AddControl(app_);
  AddRow(app_row);

  auto* buttons = new QWidget(this);
  auto* buttons_layout = new QHBoxLayout(buttons);
  buttons_layout->setContentsMargins(18, 12, 18, 10);
  buttons_layout->setSpacing(8);
  buttons_layout->addStretch(1);
  auto* keep = new QPushButton("Keep as is", buttons);
  connect(keep, &QPushButton::clicked, this, &InstallPromptCard::CloseRequested);
  buttons_layout->addWidget(keep);
  use_ = new QPushButton("Use this program", buttons);
  use_->setDefault(true);
  connect(use_, &QPushButton::clicked, this, [this] { emit Accepted(exe_path_, app_->isChecked()); });
  buttons_layout->addWidget(use_);
  AddRow(buttons);

  ShowProgram();
}

void InstallPromptCard::ShowProgram() {
  const bool found = !exe_path_.empty();
  program_->setText(found ? WindowsPath((std::filesystem::path(install_path_) / exe_path_).string())
                          : QString("None found"));
  theme::SetStyleProperty(program_, "role", found ? "" : "subtle");
  use_->setEnabled(found);
}

void InstallPromptCard::Choose() {
  const QString selected = QFileDialog::getOpenFileName(this, "Select the installed program",
                                                        QString::fromStdString(install_path_),
                                                        "Programs (*.exe);;All files (*)");
  if (selected.isEmpty()) return;
  std::error_code ec;
  const std::filesystem::path relative = std::filesystem::relative(selected.toStdString(), install_path_, ec);
  if (ec || relative.empty() || relative.native().starts_with("..")) {
    notify::Notice(this, "Pick a program inside " + WindowsPath(install_path_) + ".", notify::Level::Warning);
    return;
  }
  exe_path_ = relative.string();
  ShowProgram();
}

}  // namespace mira_gui
