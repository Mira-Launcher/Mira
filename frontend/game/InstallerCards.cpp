#include "InstallerCards.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <algorithm>

#include "../app/ErrorHelp.h"
#include "../client/api/Games.h"
#include "../components/Button.h"
#include "../components/FlatCombo.h"
#include "../components/Format.h"
#include "../components/Labels.h"
#include "../components/SettingRow.h"
#include "../library/ArtworkStore.h"
#include "../theme/Icons.h"
#include "CardParts.h"
#include "LaunchOptionsEdit.h"

namespace mira_gui {

InstallerLeftoverCard::InstallerLeftoverCard(const GameSummary& game,
                                             const std::string& installer_dir, std::int64_t bytes,
                                             ArtworkStore* artwork, QWidget* parent)
    : SheetCard(QString(), parent), game_id_(game.id) {
  setFixedWidth(560);
  AddGameCardHeading(this, game, "Delete " + QString::fromStdString(game.name) + "'s installer?",
                     artwork);
  SetClose("Keep it", [this] { emit CloseRequested(); });

  auto* folder = new SettingRow("Installer folder", QString(), this);
  auto* path = new QLabel(
      QString("%1  ·  %2").arg(QString::fromStdString(installer_dir), SizeText(bytes)), folder);
  path->setWordWrap(true);
  path->setTextInteractionFlags(Qt::TextSelectableByMouse);
  folder->SetBelow(path);
  AddRow(folder);
  error_ = ErrorLine();

  QHBoxLayout* buttons = ButtonRow();
  auto* keep = new Button("Keep", ButtonKind::Primary, this);
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
    : SheetCard(QString(), parent), game_id_(game.id), current_(std::move(current)) {
  setFixedWidth(560);
  AddGameCardHeading(this, game,
                     QString::fromStdString(event.by) + " installed " +
                         (event.programs.size() == 1 ? "a program" : "programs"),
                     artwork);
  SetClose("Close", [this] { emit CloseRequested(); });
  for (const ProgramsFoundEvent::Program& program : event.programs) {
    auto* row = new SettingRow(QString::fromStdString(program.name), QString(), this);
    auto* path = new QLabel(QString::fromStdString(program.exe), row);
    path->setWordWrap(true);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    SetTextStyle(path, text::kSubtle);
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
          // Nothing left to add: done.
          if (std::ranges::all_of(adds_, [](QPushButton* b) { return b->text() == "Added"; }))
            return emit CloseRequested();
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
  error_ = ErrorLine();

  QHBoxLayout* buttons = ButtonRow();
  auto* done = new Button("Done", ButtonKind::Primary, this);
  done->setDefault(true);
  connect(done, &QPushButton::clicked, this, &ProgramsFoundCard::CloseRequested);
  buttons->addWidget(done);
}

}  // namespace mira_gui
