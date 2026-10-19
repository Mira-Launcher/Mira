#include "PrefixCards.h"

#include <QAction>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <utility>

#include "../app/ErrorHelp.h"
#include "../client/EventHub.h"
#include "../client/Events.h"
#include "../client/api/Games.h"
#include "../dialogs/LogWindow.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/FlowLayout.h"
#include "../widgets/Labels.h"
#include "../widgets/PathField.h"
#include "../widgets/ProgressRail.h"
#include "../widgets/WindowCards.h"
#include "CardParts.h"

namespace mira_gui {
namespace {

constexpr int kWidth = 520;

// No catalog endpoint exists, just a shortlist of common verbs, alongside the free-text field.
constexpr const char* kCommonVerbs[] = {
    "corefonts", "vcrun2022", "vcrun2019", "dotnet48", "dotnet6",
    "d3dx9",     "d3dx11_43", "xact",      "physx",    "gdiplus",
};

// The button row, with a status line at its left and a rail over it that keeps its place while
// hidden, so the card never changes height. Returns the row's layout for the buttons.
QHBoxLayout* StatusButtonRow(SettingsCard* card, ProgressRail*& rail, QLabel*& status,
                             QPushButton** trailing = nullptr) {
  auto* row = new QWidget(card);
  auto* layout = new QVBoxLayout(row);
  layout->setContentsMargins(18, 8, 18, 10);
  layout->setSpacing(8);
  rail = new ProgressRail(row);
  rail->SetProgress(-1);
  KeepSpaceWhenHidden(rail);
  rail->hide();
  layout->addWidget(rail);
  auto* line = new QHBoxLayout();
  line->setSpacing(8);
  status = new QLabel(row);
  status->setProperty("role", "muted");
  status->setWordWrap(true);
  line->addWidget(status, 1);
  if (trailing != nullptr) {
    *trailing = new QPushButton(row);
    (*trailing)->setFlat(true);
    line->addWidget(*trailing);
  }
  layout->addLayout(line);
  card->AddFooterRow(row);
  return line;
}

}  // namespace

RunInPrefixCard::RunInPrefixCard(std::string game_id, std::string install_path, const QString& name,
                                 QWidget* parent)
    : SettingsCard(QString(), parent),
      game_id_(std::move(game_id)),
      install_path_(std::move(install_path)) {
  setFixedWidth(kWidth);
  SetProminentTitle();
  AddCardHeading(this, "Run in " + name + "'s prefix", "Close", [this] { cards::Close(this); });

  auto* program_row = new SettingRow("Program", QString(), this);
  program_ = new QLineEdit(program_row);
  program_->setPlaceholderText("Absolute, or in its install folder");
  program_row->AddWideControl(program_);
  // Choose sits inside the field, so both fields share their edges.
  QAction* choose =
      program_->addAction(icons::For(icons::Glyph::Folder), QLineEdit::TrailingPosition);
  choose->setToolTip("Choose…");
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, choose,
          [choose] { choose->setIcon(icons::For(icons::Glyph::Folder)); });
  connect(choose, &QAction::triggered, this, [this] {
    const QString picked = QFileDialog::getOpenFileName(this, "The program to run",
                                                        QString::fromStdString(install_path_));
    if (!picked.isEmpty()) program_->setText(RelativeIfInside(picked, install_path_));
  });
  AddRow(program_row);

  auto* args_row = new SettingRow("Arguments", QString(), this);
  args_ = new QLineEdit(args_row);
  args_->setPlaceholderText("/S");
  args_row->AddWideControl(args_);
  AddRow(args_row);
  const int label_width =
      std::max(program_row->Label()->sizeHint().width(), args_row->Label()->sizeHint().width());
  program_row->Label()->setMinimumWidth(label_width);
  args_row->Label()->setMinimumWidth(label_width);

  QHBoxLayout* buttons = StatusButtonRow(this, rail_, status_);

  auto* cancel = new QPushButton("Cancel", this);
  connect(cancel, &QPushButton::clicked, this, [this] { cards::Close(this); });
  buttons->addWidget(cancel);
  run_ = new QPushButton("Run", this);
  run_->setDefault(true);
  run_->setEnabled(false);  // nothing to run until a program is picked
  connect(run_, &QPushButton::clicked, this, &RunInPrefixCard::Run);
  buttons->addWidget(run_);
  connect(program_, &QLineEdit::textChanged, this,
          [this](const QString& text) { run_->setEnabled(!text.trimmed().isEmpty()); });
  connect(program_, &QLineEdit::returnPressed, run_, &QPushButton::click);
  connect(args_, &QLineEdit::returnPressed, run_, &QPushButton::click);
}

void RunInPrefixCard::Run() {
  const std::string program = program_->text().trimmed().toStdString();
  if (program.empty()) return;
  // Making a prefix takes a while, and the reply waits until the program has started.
  for (QWidget* widget : std::initializer_list<QWidget*>{program_, args_, run_})
    widget->setEnabled(false);
  rail_->show();
  theme::SetStyleProperty(status_, "role", "muted");
  status_->setText("Starting it, and making the prefix first if there's none…");
  api::RunInPrefixAsync(
      this, game_id_, program, args_->text().toStdString(), [this](RunInPrefixResult result) {
        if (result.ok) return cards::Close(this);
        for (QWidget* widget : std::initializer_list<QWidget*>{program_, args_, run_})
          widget->setEnabled(true);
        rail_->hide();
        theme::SetStyleProperty(status_, "role", "error");
        status_->setText("Could not run it: " + error_help::Describe(result.error));
      });
}

WinetricksCard::WinetricksCard(std::string game_id, const QString& name, QWidget* parent)
    : SettingsCard(QString(), parent), game_id_(std::move(game_id)), name_(name) {
  setFixedWidth(kWidth);
  SetProminentTitle();
  AddCardHeading(this, "Winetricks for " + name, "Close", [this] { cards::Close(this); });

  auto* pick = new QWidget(this);
  auto* pick_layout = new QVBoxLayout(pick);
  pick_layout->setContentsMargins(18, 10, 18, 10);
  pick_layout->setSpacing(10);
  verb_ = new QLineEdit(pick);
  verb_->setPlaceholderText("Search or type a verb");
  verb_->setClearButtonEnabled(true);
  pick_layout->addWidget(verb_);
  chips_ = new QWidget(pick);
  chips_layout_ = new FlowLayout(chips_);
  pick_layout->addWidget(chips_);
  AddRow(pick);

  QHBoxLayout* buttons = StatusButtonRow(this, rail_, status_, &log_);
  log_->setText("View log");
  log_->hide();
  connect(log_, &QPushButton::clicked, this,
          [this] { LogWindow::Open(this, "game:" + QString::fromStdString(game_id_), name_); });

  auto* close = new QPushButton("Close", this);
  connect(close, &QPushButton::clicked, this, [this] { cards::Close(this); });
  buttons->addWidget(close);
  run_ = new QPushButton("Run", this);
  run_->setDefault(true);
  run_->setEnabled(false);
  connect(run_, &QPushButton::clicked, this, &WinetricksCard::Run);
  buttons->addWidget(run_);

  connect(verb_, &QLineEdit::textChanged, this, [this] {
    RebuildChips();
    SetRunning(running_);
  });
  connect(verb_, &QLineEdit::returnPressed, run_, &QPushButton::click);
  RebuildChips();

  api::GetInstalledTricksAsync(this, game_id_, [this](InstalledTricksResult result) {
    if (!result.ok) return;  // the chips just go unticked
    installed_.insert(result.installed.begin(), result.installed.end());
    RebuildChips();
  });
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live) HandleEvent(type, data);
          });
  // Its finish event may have been missed while disconnected: unlock rather than wait forever.
  connect(EventHub::Instance(), &EventHub::ConnectionChanged, this, [this](bool connected) {
    if (connected || !running_) return;
    SetRunning(false);
    SetStatus("Lost contact with mirad while it ran. Check the game before running it again.",
              /*error=*/true);
  });
}

// Those already in the prefix first, then the shortlist, narrowed to what the field holds.
void WinetricksCard::RebuildChips() {
  // Later, not now: a chip's own click is what got here.
  while (QLayoutItem* item = chips_layout_->takeAt(0)) {
    item->widget()->hide();
    item->widget()->deleteLater();
    delete item;
  }
  std::vector<std::string> verbs(installed_.begin(), installed_.end());
  for (const char* verb : kCommonVerbs) {
    if (!installed_.contains(verb)) verbs.emplace_back(verb);
  }
  const QString typed = verb_->text().trimmed();
  for (const std::string& verb : verbs) {
    const QString text = QString::fromStdString(verb);
    if (!typed.isEmpty() && !text.contains(typed, Qt::CaseInsensitive)) continue;
    auto* chip = new QPushButton(text, chips_);
    chip->setObjectName("chip");
    chip->setCheckable(true);
    chip->setChecked(text == typed);
    if (installed_.contains(verb)) {
      icons::Follow(chip, icons::Glyph::CheckCircle, &theme::Tokens::success);
      chip->setToolTip("Already in the prefix; runs it again");
    }
    connect(chip, &QPushButton::clicked, this, [this, text] { verb_->setText(text); });
    chips_layout_->addWidget(chip);
  }
}

void WinetricksCard::SetRunning(bool running) {
  running_ = running;
  verb_->setEnabled(!running);
  chips_->setEnabled(!running);
  run_->setEnabled(!running && !verb_->text().trimmed().isEmpty());
  rail_->setVisible(running);
}

void WinetricksCard::SetStatus(const QString& text, bool error) {
  status_->setText(text);
  theme::SetStyleProperty(status_, "role", error ? "error" : "muted");
}

void WinetricksCard::Run() {
  const std::string verb = verb_->text().trimmed().toStdString();
  if (verb.empty() || running_) return;
  SetRunning(true);
  log_->show();
  SetStatus(QString("Starting %1…").arg(QString::fromStdString(verb)));
  api::RunWinetricksAsync(this, game_id_, verb, [this, verb](TricksResult result) {
    if (!result.ok) {
      SetRunning(false);
      SetStatus(QString("Could not start %1: %2")
                    .arg(QString::fromStdString(verb), error_help::Describe(result.error)),
                /*error=*/true);
      return;
    }
    if (running_) SetStatus(QString("Running %1…").arg(QString::fromStdString(verb)));
  });
}

void WinetricksCard::HandleEvent(const std::string& type, const std::string& data) {
  TricksEvent event;
  if (!events::ParseTricksEvent(type, data, &event) || event.id != game_id_) return;
  const QString verb = QString::fromStdString(event.verb);
  if (event.state == "started") {
    SetStatus(QString("Running %1…").arg(verb));
    return;
  }
  SetRunning(false);
  if (event.state == "failed") {
    SetStatus(QString("%1 failed: %2").arg(verb, error_help::Describe(event.error)),
              /*error=*/true);
    return;
  }
  installed_.insert(event.verb);
  RebuildChips();
  SetStatus(QString("%1 finished.").arg(verb));
}

}  // namespace mira_gui
