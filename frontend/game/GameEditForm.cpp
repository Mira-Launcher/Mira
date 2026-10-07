#include "GameEditForm.h"

#include "../client/Async.h"
#include "../client/EventHub.h"
#include "../client/api/Config.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../client/api/Runners.h"
#include "../client/JsonMapping.h"
#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../settings/SettingEditor.h"
#include "../settings/SettingsCard.h"
#include "../widgets/KeyValueEdit.h"
#include "../widgets/PathField.h"
#include "../widgets/Scrolling.h"
#include "../widgets/TagEdit.h"
#include "OverridesEditor.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

namespace mira_gui {
namespace {

// A field with its buttons after it, for under a row's label.
QWidget* FieldLine(QWidget* parent, QWidget* field, std::initializer_list<QWidget*> buttons) {
  auto* line = new QWidget(parent);
  auto* layout = new QHBoxLayout(line);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  layout->addWidget(field, /*stretch=*/1);
  for (QWidget* button : buttons) layout->addWidget(button);
  return line;
}

QComboBox* EditableCombo(QWidget* parent, const QString& placeholder) {
  auto* combo = new QComboBox(parent);
  combo->setEditable(true);
  combo->setInsertPolicy(QComboBox::NoInsert);
  combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  combo->lineEdit()->setPlaceholderText(placeholder);
  return combo;
}

QLineEdit* LineEdit(QWidget* parent, const QString& placeholder = {}) {
  auto* edit = new QLineEdit(parent);
  edit->setPlaceholderText(placeholder);
  return edit;
}

}  // namespace

GameEditForm::GameEditForm(std::string id, QWidget* parent) : QWidget(parent), id_(std::move(id)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  pages_ = new QStackedWidget(this);
  layout->addWidget(pages_);

  auto* scroll = new QScrollArea(pages_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
  scroll->viewport()->setAutoFillBackground(false);
  SetUpScrolling(scroll);
  auto* cards_page = new QWidget();
  auto* cards_layout = new QVBoxLayout(cards_page);
  cards_layout_ = cards_layout;
  cards_layout->setContentsMargins(18, 18, 18, 18);
  cards_layout->setSpacing(14);
  scroll->setWidget(cards_page);
  pages_->addWidget(scroll);

  last_error_label_ = new QLabel(cards_page);
  last_error_label_->setWordWrap(true);
  last_error_label_->setProperty("role", "error");
  last_error_label_->hide();
  cards_layout->addWidget(last_error_label_);

  // --- Launch
  auto* launch = new SettingsCard("Launch", cards_page);
  exe_combo_ = EditableCombo(launch, QString());
  connect(exe_combo_, &QComboBox::activated, this, &GameEditForm::OnExeComboActivated);
  auto* browse = new QPushButton("Browse…", launch);
  connect(browse, &QPushButton::clicked, this, &GameEditForm::BrowseExecutable);
  // Shown while mirad wants its pick confirmed; saving any change confirms it too.
  check_tag_ = new QLabel("Not checked", launch);
  check_tag_->setObjectName("check_tag");
  check_tag_->setToolTip("Mira wasn't sure which program starts this game");
  check_tag_->hide();
  looks_right_ = new QPushButton("Looks right", launch);
  looks_right_->setToolTip("Keep this executable and stop asking");
  looks_right_->hide();
  connect(looks_right_, &QPushButton::clicked, this, &GameEditForm::ConfirmExecutable);
  // Shown for a store that launches the game its own way, where the field isn't what runs.
  source_note_label_ = new QLabel(launch);
  source_note_label_->setWordWrap(true);
  source_note_label_->setProperty("role", "muted");
  source_note_label_->hide();
  auto* exe_box = new QWidget(launch);
  auto* exe_box_layout = new QVBoxLayout(exe_box);
  exe_box_layout->setContentsMargins(0, 0, 0, 0);
  exe_box_layout->setSpacing(6);
  exe_box_layout->addWidget(FieldLine(exe_box, exe_combo_, {browse, looks_right_}));
  exe_box_layout->addWidget(source_note_label_);
  exe_row_ = new SettingRow("Executable", QString(), launch);
  exe_row_->AddAfterLabel(check_tag_);
  exe_row_->SetBelow(exe_box);
  launch->AddRow(exe_row_);

  args_edit_ = LineEdit(launch, "None");
  args_row_ = new SettingRow("Arguments", QString(), launch);
  args_row_->SetBelow(args_edit_);
  launch->AddRow(args_row_);

  working_dir_edit_ = LineEdit(launch, "The install folder");
  working_dir_row_ = new SettingRow("Working folder", QString(), launch);
  working_dir_row_->SetBelow(working_dir_edit_);
  launch->AddRow(working_dir_row_);

  // --- Runner
  auto* runner = new SettingsCard("Runner", cards_page);
  runner_combo_ = EditableCombo(runner, "Default runner");
  runner_row_ = new SettingRow("Runner", QString(), runner);
  runner_row_->SetBelow(runner_combo_);
  runner->AddRow(runner_row_);

  // Under its label like the other rows, so a narrow card never squeezes the label.
  auto* advanced = new QPushButton("Advanced settings…", runner);
  connect(advanced, &QPushButton::clicked, this, &GameEditForm::OpenAdvanced);
  auto* advanced_line = new QWidget(runner);
  auto* advanced_line_layout = new QHBoxLayout(advanced_line);
  advanced_line_layout->setContentsMargins(0, 0, 0, 0);
  advanced_line_layout->addWidget(advanced);
  advanced_line_layout->addStretch(1);
  auto* advanced_row = new SettingRow("Environment, runner options and scripts", QString(), runner);
  advanced_row->SetBelow(advanced_line);
  runner->AddRow(advanced_row);

  // --- Details
  auto* details = new SettingsCard("Details", cards_page);
  name_edit_ = LineEdit(details);
  name_row_ = new SettingRow("Name", QString(), details);
  name_row_->SetBelow(name_edit_);
  details->AddRow(name_row_);

  tags_edit_ = new TagEdit(details);
  connect(tags_edit_, &TagEdit::TagClicked, this, &GameEditForm::TagFilterRequested);
  tags_row_ = new SettingRow(
      "Tags",
      "Labels of your choice. The \"hidden\" tag keeps this game out of the library until you ask for it "
      "(Ctrl+H, or the Hidden filter). In a library folder sorted by tag, the outlined tag is the folder "
      "the game is in; drag another folder tag ahead of it to move the game. Folder tags are chosen on the "
      "Tags page.",
      details);
  tags_row_->SetBelow(tags_edit_);
  details->AddRow(tags_row_);

  // --- Files
  auto* files = new SettingsCard("Files", cards_page);
  // Read-only: editing the path would only repoint the record, Move… moves the files too.
  install_path_edit_ = LineEdit(files, "Not installed");
  install_path_edit_->setReadOnly(true);
  move_button_ = new QPushButton("Move…", files);
  move_button_->setToolTip("Move this game's files to another folder");
  connect(move_button_, &QPushButton::clicked, this, [this] { MoveFolder(/*prefix=*/false); });
  auto* install_row = new SettingRow("Install folder", QString(), files);
  install_row->SetBelow(FieldLine(files, install_path_edit_, {move_button_}));
  files->AddRow(install_row);

  data_dir_edit_ = LineEdit(files, "Automatic");
  open_data_dir_ = new QPushButton("Open", files);
  open_data_dir_->setToolTip("Open this folder in the file manager");
  open_data_dir_->setEnabled(false);
  connect(open_data_dir_, &QPushButton::clicked, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(data_dir_edit_->text()));
  });
  // Editing the path repoints the record to another prefix; Move… moves this one.
  move_prefix_ = new QPushButton("Move…", files);
  move_prefix_->setToolTip("Move this game's prefix to another folder");
  move_prefix_->setVisible(false);
  connect(move_prefix_, &QPushButton::clicked, this, [this] { MoveFolder(/*prefix=*/true); });
  data_dir_row_ = new SettingRow("Prefix and data folder",
                                 "The folder holding this game's Wine or Proton prefix and its data.", files);
  data_dir_row_->SetBelow(FieldLine(files, data_dir_edit_, {open_data_dir_, move_prefix_}));
  files->AddRow(data_dir_row_);

  // Two columns stacked separately, not a grid, so a short card leaves no gap under it.
  auto* columns = new QHBoxLayout();
  columns->setSpacing(14);
  for (const auto& [top, bottom] : {std::pair{launch, details}, std::pair{runner, files}}) {
    auto* column = new QVBoxLayout();
    column->setSpacing(14);
    column->addWidget(top);
    column->addWidget(bottom);
    column->addStretch(1);
    columns->addLayout(column, /*stretch=*/1);
  }
  cards_layout->addLayout(columns, /*stretch=*/1);

  // --- Advanced: the runner options page, then the overrides' own categories.
  overrides_ = new mira_gui::OverridesEditor(id_, pages_);
  SettingsPage* options_page = overrides_->AddPage("Runner options", icons::Glyph::Sliders);
  runner_options_card_ = options_page->AddCard(QString());  // titled and filled per runner kind
  runner_options_card_->hide();
  SettingsCard* env_card = options_page->AddCard(QString());
  env_edit_ = new KeyValueEdit("Add variable", env_card);
  env_row_ = new SettingRow("Environment variables",
                            "Added to the runner's own environment. A variable set here wins over both.", env_card);
  env_row_->SetBelow(env_edit_);
  env_card->AddRow(env_row_);
  pages_->addWidget(overrides_);
  connect(overrides_, &OverridesEditor::Changed, this, &GameEditForm::Changed);

  for (QLineEdit* edit : {name_edit_, args_edit_, working_dir_edit_, data_dir_edit_}) {
    connect(edit, &QLineEdit::textChanged, this, &GameEditForm::UpdateModified);
  }
  for (QComboBox* combo : {exe_combo_, runner_combo_}) {
    connect(combo, &QComboBox::editTextChanged, this, &GameEditForm::UpdateModified);
  }
  connect(runner_combo_, &QComboBox::editTextChanged, this, &GameEditForm::ShowRunnerOptions);
  connect(env_edit_, &KeyValueEdit::Changed, this, &GameEditForm::UpdateModified);
  connect(tags_edit_, &TagEdit::Changed, this, &GameEditForm::UpdateModified);
  const auto revert = [this](SettingRow* row, auto member) {
    connect(row, &SettingRow::RevertClicked, this, [this, member] {
      mira_gui::GamePatch patch = CurrentPatch();
      patch.*member = original_patch_.*member;
      populating_ = true;
      ShowPatch(patch);
      populating_ = false;
      UpdateModified();
    });
  };
  revert(name_row_, &mira_gui::GamePatch::name);
  revert(exe_row_, &mira_gui::GamePatch::exe_path);
  revert(args_row_, &mira_gui::GamePatch::args);
  revert(working_dir_row_, &mira_gui::GamePatch::working_dir);
  revert(tags_row_, &mira_gui::GamePatch::tags);
  revert(runner_row_, &mira_gui::GamePatch::runner_ref);
  revert(data_dir_row_, &mira_gui::GamePatch::data_dir);
  revert(env_row_, &mira_gui::GamePatch::env_json);
  // Checked once typing pauses and off the UI thread: a stale network mount can stall a stat.
  auto* check_data_dir = new QTimer(this);
  check_data_dir->setSingleShot(true);
  check_data_dir->setInterval(300);
  connect(check_data_dir, &QTimer::timeout, this, [this] {
    const QString path = data_dir_edit_->text();
    async::Run<bool>(
        this, [path] { return QDir(path).exists(); },
        [this, path](bool exists) {
          if (data_dir_edit_->text() == path) open_data_dir_->setEnabled(exists);
        });
  });
  connect(data_dir_edit_, &QLineEdit::textChanged, this,
          [this, check_data_dir](const QString& path) {
            open_data_dir_->setEnabled(false);
            if (!path.isEmpty()) check_data_dir->start();
          });

  setEnabled(false);
  Load();
  connect(EventHub::Instance(), &EventHub::RunnersChanged, this, [this] {
    api::ListRunnersAsync(this, [this](RunnersResult result) { PopulateRunnerCombo(result); });
  });
}

void GameEditForm::SetTagSuggestions(const QStringList& tags) { tags_edit_->SetSuggestions(tags); }

void GameEditForm::SetTagOrder(std::vector<std::string> order) { tags_edit_->SetOrder(std::move(order)); }

void GameEditForm::SetBottomRoom(int height) {
  const QMargins margins = cards_layout_->contentsMargins();
  cards_layout_->setContentsMargins(margins.left(), margins.top(), margins.right(), 18 + height);
}

void GameEditForm::OpenAdvanced() {
  pages_->setCurrentIndex(1);
  overrides_->ShowFromTop();  // the runner's card may have come or gone since last time
  emit AdvancedChanged(true);
}

void GameEditForm::CloseAdvanced() {
  pages_->setCurrentIndex(0);
  ResetScroll();
  emit AdvancedChanged(false);
}

bool GameEditForm::AdvancedOpen() const { return pages_->currentIndex() == 1; }

void GameEditForm::ResetScroll() {
  if (auto* scroll = qobject_cast<QScrollArea*>(pages_->widget(0))) scroll->verticalScrollBar()->setValue(0);
}

void GameEditForm::Reload() {
  if (!IsDirty()) Load();
}

void GameEditForm::Load() {
  mira_gui::api::ListRunnersAsync(this, [this](mira_gui::RunnersResult result) {
    PopulateRunnerCombo(result);
  });
  mira_gui::api::GetGameAsync(this, id_, [this](mira_gui::GameDetailResult result) {
    if (!result.ok) {
      emit LoadFailed(error_help::Describe(result.error));
      return;
    }
    setEnabled(true);
    Populate(result.game);
  });
  overrides_->Load();
}

void GameEditForm::Populate(const mira_gui::GameDetail& game) {
  emit Loaded(QString::fromStdString(game.name));

  install_path_ = game.install_path;
  ShowInstallPath();
  // A desktop entry's files belong to another app; Steam moves its own games.
  move_button_->setVisible(!game.install_path.empty() && game.source != "desktop-entry" && game.source != "steam");

  if (game.source == "steam") {
    source_note_label_->setText(
        "Steam launches this game from its own record of the executable, so changing it here has no effect.");
  }
  source_note_label_->setVisible(game.source == "steam");

  check_tag_->setVisible(game.needs_check);
  looks_right_->setVisible(game.needs_check);

  last_error_label_->setText(QString("Error: %1").arg(QString::fromStdString(game.last_error)));
  last_error_label_->setVisible(!game.last_error.empty());

  populating_ = true;
  PopulateExeCombo(game.candidates, game.exe_path);
  mira_gui::GamePatch patch;
  patch.name = game.name;
  patch.exe_path = game.exe_path;
  patch.args = game.args;
  patch.working_dir = game.working_dir;
  patch.tags = game.tags;
  patch.runner_ref = game.runner_ref;
  patch.data_dir = game.data_dir;
  patch.runner_config_json = game.runner_config_json;
  patch.env_json = game.env_json;
  default_runner_ = game.default_runner;
  tags_edit_->SetFolderTags(game.folder_tags);
  original_pick_ = game.folder_tag;
  ShowPatch(patch);
  tags_edit_->SetFolderPick(original_pick_);
  populating_ = false;

  original_patch_ = CurrentPatch();
  move_prefix_->setVisible(!game.data_dir.empty() && game.source != "desktop-entry");
  ShowRunnerOptions();
  UpdateModified();
}

void GameEditForm::ShowPatch(const mira_gui::GamePatch& patch) {
  const auto show = [](QLineEdit* edit, const std::optional<std::string>& value) {
    edit->setText(QString::fromStdString(value.value_or(std::string())));
    edit->setCursorPosition(0);
  };
  show(name_edit_, patch.name);
  show(args_edit_, patch.args);
  show(working_dir_edit_, patch.working_dir);
  show(data_dir_edit_, patch.data_dir);
  show(exe_combo_->lineEdit(), patch.exe_path);
  ShowRunnerRef(runner_combo_, QString::fromStdString(patch.runner_ref.value_or(std::string())));
  tags_edit_->SetTags(patch.tags.value_or(std::vector<std::string>()));
  runner_config_ = nlohmann::json::parse(patch.runner_config_json.value_or("{}"), nullptr, false);
  if (!runner_config_.is_object()) runner_config_ = nlohmann::json::object();
  ShowRunnerFieldValues();
  std::map<std::string, std::string> env;
  const nlohmann::json env_json = nlohmann::json::parse(patch.env_json.value_or("{}"), nullptr, false);
  if (env_json.is_object()) {
    for (const auto& [key, value] : env_json.items()) env[key] = value.is_string() ? value.get<std::string>() : value.dump();
  }
  env_edit_->SetValues(env);
}

std::string GameEditForm::RunnerKind() const {
  std::string ref = RunnerRef(runner_combo_).toStdString();
  if (ref.empty()) ref = default_runner_;
  return ref.substr(0, ref.find(':'));
}

void GameEditForm::ShowRunnerOptions() {
  const std::string kind = RunnerKind();
  if (kind == options_kind_) return;
  options_kind_ = kind;
  if (const auto known = runner_schemas_.find(kind); known != runner_schemas_.end() || kind.empty()) {
    BuildRunnerFields(kind.empty() ? std::vector<RunnerOption>() : known->second);
    return;
  }
  api::GetRunnerSchemaAsync(this, kind, [this, kind](RunnerSchemaResult result) {
    // An unknown kind (a typed-in reference) simply has no options to show.
    runner_schemas_[kind] = result.ok ? result.options : std::vector<RunnerOption>();
    if (kind == options_kind_) BuildRunnerFields(runner_schemas_[kind]);
  });
}

void GameEditForm::BuildRunnerFields(const std::vector<RunnerOption>& options) {
  runner_options_card_->ClearRows();
  runner_fields_.clear();
  QString title = QString::fromStdString(options_kind_);
  if (!title.isEmpty()) title[0] = title[0].toUpper();
  runner_options_card_->SetTitle(title);
  for (const RunnerOption& option : options) {
    auto* row = new SettingRow(QString::fromStdString(option.label), QString::fromStdString(option.doc),
                               runner_options_card_);
    QLineEdit* edit = LineEdit(runner_options_card_, "Not set");
    row->SetBelow(edit);
    runner_options_card_->AddRow(row);
    const std::string key = option.key;
    connect(edit, &QLineEdit::textEdited, this, [this, key](const QString& text) {
      if (text.isEmpty()) {
        runner_config_.erase(key);
      } else {
        runner_config_[key] = text.toStdString();
      }
      UpdateModified();
    });
    connect(row, &SettingRow::RevertClicked, this, [this, key] {
      const nlohmann::json saved = nlohmann::json::parse(original_patch_.runner_config_json.value_or("{}"), nullptr, false);
      if (saved.is_object() && saved.contains(key)) {
        runner_config_[key] = saved[key];
      } else {
        runner_config_.erase(key);
      }
      ShowRunnerFieldValues();
      UpdateModified();
    });
    runner_fields_.push_back({key, row, edit});
  }
  runner_options_card_->setVisible(!options.empty());
  ShowRunnerFieldValues();
  UpdateModified();
}

void GameEditForm::ShowRunnerFieldValues() {
  for (const RunnerField& field : runner_fields_) {
    const auto it = runner_config_.find(field.key);
    field.edit->setText(it == runner_config_.end() ? QString()
                        : it->is_string()          ? QString::fromStdString(it->get<std::string>())
                                                   : QString::fromStdString(it->dump()));
    field.edit->setCursorPosition(0);
  }
}

void GameEditForm::ShowInstallPath() {
  install_path_edit_->setText(QString::fromStdString(install_path_));
  install_path_edit_->setCursorPosition(0);
  install_path_edit_->setToolTip(install_path_edit_->text());
}

void GameEditForm::PopulateExeCombo(const std::vector<mira_gui::GameDetail::Candidate>& candidates,
                                    const std::string& current) {
  std::vector<mira_gui::GameDetail::Candidate> sorted = candidates;
  std::ranges::sort(sorted, std::greater{}, &mira_gui::GameDetail::Candidate::score);

  exe_combo_->blockSignals(true);
  exe_combo_->clear();
  for (const mira_gui::GameDetail::Candidate& candidate : sorted) {
    QString label = QString("%1 (%2, score %3)")
                        .arg(QString::fromStdString(candidate.rel_path),
                             QString::fromStdString(candidate.kind))
                        .arg(candidate.score, 0, 'f', 1);
    if (candidate.is_installer) label += " (installer, probably not the game)";
    exe_combo_->addItem(label, QString::fromStdString(candidate.rel_path));
  }
  exe_combo_->setEditText(QString::fromStdString(current));
  exe_combo_->lineEdit()->setCursorPosition(0);
  exe_combo_->blockSignals(false);
}

void GameEditForm::PopulateRunnerCombo(const mira_gui::RunnersResult& result) {
  const QString current = RunnerRef(runner_combo_);
  runner_combo_->blockSignals(true);
  runner_combo_->clear();
  runner_combo_->addItem("Default runner", QString());
  if (result.ok) {
    for (const mira_gui::RunnerInfo& runner : result.runners) {
      const QString label = QString("%1 (%2)").arg(QString::fromStdString(runner.name),
                                                     QString::fromStdString(runner.kind));
      runner_combo_->addItem(label, QString::fromStdString(runner.reference));
    }
  }
  ShowRunnerRef(runner_combo_, current);
  runner_combo_->blockSignals(false);
}

void GameEditForm::OnExeComboActivated(int index) {
  exe_combo_->setEditText(exe_combo_->itemData(index).toString());
  exe_combo_->lineEdit()->setCursorPosition(0);
}

void GameEditForm::BrowseExecutable() {
  const QString start_dir = install_path_.empty() ? QString() : QString::fromStdString(install_path_);
  const QString selected = QFileDialog::getOpenFileName(this, "Select executable", start_dir);
  if (selected.isEmpty()) return;

  exe_combo_->setEditText(RelativeIfInside(selected, install_path_));
  exe_combo_->lineEdit()->setCursorPosition(0);
}

void GameEditForm::MoveFolder(bool prefix) {
  const std::string current = prefix ? original_patch_.data_dir.value_or(std::string()) : install_path_;
  const QString parent_dir =
      current.empty() ? QString() : QString::fromStdString(std::filesystem::path(current).parent_path().string());
  const QString picked = QFileDialog::getExistingDirectory(
      this, prefix ? "Move the prefix folder into" : "Move the game's folder into", parent_dir);
  if (picked.isEmpty()) return;
  const std::string target =
      (std::filesystem::path(picked.toStdString()) / std::filesystem::path(current).filename()).string();
  if (target == current) return;
  const QString what = prefix ? "this game's prefix" : "this game's files";
  if (!notify::Confirm(this, prefix ? "Move prefix" : "Move game",
                       QString("Move %1 to %2? A move to another drive copies them first, which can take a while.")
                           .arg(what, QString::fromStdString(target)),
                       "Move")) {
    return;
  }
  QPushButton* button = prefix ? move_prefix_ : move_button_;
  button->setEnabled(false);
  button->setText("Moving…");
  api::RelocateGameAsync(this, id_, prefix ? std::string() : target, prefix ? target : std::string(),
                                 [this, button, prefix](GameDetailResult result) {
                                   button->setText("Move…");
                                   if (!result.ok) {
                                     button->setEnabled(true);
                                     notify::FailedRequest(
                                         this, prefix ? "Could not move the prefix." : "Could not move the game.",
                                         result.error);
                                     return;
                                   }
                                   install_path_ = result.game.install_path;
                                   ShowInstallPath();
                                   // Moved on disk and saved, so not an unsaved change.
                                   original_patch_.data_dir = result.game.data_dir;
                                   populating_ = true;
                                   data_dir_edit_->setText(QString::fromStdString(result.game.data_dir));
                                   data_dir_edit_->setCursorPosition(0);
                                   populating_ = false;
                                   button->setEnabled(true);
                                   UpdateModified();
                                 });
}

void GameEditForm::ConfirmExecutable() {
  mira_gui::GamePatch patch;
  patch.reviewed = true;
  looks_right_->setEnabled(false);
  api::PatchGameAsync(this, id_, patch, [this](PatchGameResult result) {
    looks_right_->setEnabled(true);
    if (!result.ok) {
      notify::FailedRequest(this, "Could not confirm the executable.", result.error);
      return;
    }
    check_tag_->hide();
    looks_right_->hide();
  });
}

mira_gui::GamePatch GameEditForm::CurrentPatch() const {
  mira_gui::GamePatch patch;
  patch.name = name_edit_->text().toStdString();
  patch.exe_path = exe_combo_->currentText().toStdString();
  patch.args = args_edit_->text().toStdString();
  patch.working_dir = working_dir_edit_->text().toStdString();
  patch.tags = tags_edit_->Tags();
  patch.runner_ref = RunnerRef(runner_combo_).toStdString();
  patch.data_dir = data_dir_edit_->text().toStdString();
  patch.runner_config_json = runner_config_.dump(2);
  patch.env_json = nlohmann::json(env_edit_->Values()).dump(2);
  return patch;
}

void GameEditForm::UpdateModified() {
  if (populating_) return;
  const mira_gui::GamePatch now = CurrentPatch();
  name_row_->SetModified(now.name != original_patch_.name);
  exe_row_->SetModified(now.exe_path != original_patch_.exe_path);
  args_row_->SetModified(now.args != original_patch_.args);
  working_dir_row_->SetModified(now.working_dir != original_patch_.working_dir);
  tags_row_->SetModified(now.tags != original_patch_.tags ||
                         tags_edit_->FolderPick() != original_pick_);
  runner_row_->SetModified(now.runner_ref != original_patch_.runner_ref);
  data_dir_row_->SetModified(now.data_dir != original_patch_.data_dir);
  move_prefix_->setEnabled(now.data_dir == original_patch_.data_dir);  // moves the saved prefix
  const nlohmann::json saved_config =
      nlohmann::json::parse(original_patch_.runner_config_json.value_or("{}"), nullptr, false);
  for (const RunnerField& field : runner_fields_) {
    const nlohmann::json was = saved_config.is_object() ? saved_config.value(field.key, nlohmann::json()) : nlohmann::json();
    field.row->SetModified(runner_config_.value(field.key, nlohmann::json()) != was);
  }
  env_row_->SetModified(now.env_json != original_patch_.env_json);
  emit Changed();
}

int GameEditForm::ChangeCount() const {
  const mira_gui::GamePatch now = CurrentPatch();
  const int fields =
      (now.name != original_patch_.name) + (now.exe_path != original_patch_.exe_path) +
      (now.args != original_patch_.args) + (now.working_dir != original_patch_.working_dir) +
      (now.tags != original_patch_.tags || tags_edit_->FolderPick() != original_pick_) +
      (now.runner_ref != original_patch_.runner_ref) + (now.data_dir != original_patch_.data_dir) +
      (now.env_json != original_patch_.env_json);
  // One per runner option, since each is its own row.
  const int runner_options = static_cast<int>(
      mapping::MergePatchBetween(nlohmann::json::parse(original_patch_.runner_config_json.value_or("{}"), nullptr, false),
                                 runner_config_)
          .size());
  return fields + runner_options + static_cast<int>(overrides_->PendingEdits().size());
}

bool GameEditForm::IsDirty() const { return ChangeCount() > 0; }

void GameEditForm::DiscardChanges() {
  populating_ = true;
  ShowPatch(original_patch_);
  tags_edit_->SetFolderPick(original_pick_);
  populating_ = false;
  overrides_->DiscardChanges();
  UpdateModified();
}

mira_gui::GamesPatch GameEditForm::TagsPatch(const std::optional<std::vector<std::string>>& now) const {
  mira_gui::GamesPatch tags;
  tags.ids = {id_};
  const std::vector<std::string> now_tags = now.value_or(std::vector<std::string>());
  const std::vector<std::string> was_tags = original_patch_.tags.value_or(std::vector<std::string>());
  for (const std::string& tag : now_tags) {
    if (std::ranges::find(was_tags, tag) == was_tags.end()) tags.add_tags.push_back(tag);
  }
  for (const std::string& tag : was_tags) {
    if (std::ranges::find(now_tags, tag) == now_tags.end()) tags.remove_tags.push_back(tag);
  }
  // With the tags, so a pick of a tag added in this save isn't dropped for the game lacking it.
  if (tags_edit_->FolderPick() != original_pick_) tags.folder_tag = tags_edit_->FolderPick();
  return tags;
}

void GameEditForm::Save() {
  const mira_gui::GamePatch current = CurrentPatch();
  const std::vector<mira_gui::GameConfigEdit> override_edits = overrides_->PendingEdits();

  // Only what changed here is sent, so a pin, hide or rename made elsewhere
  // while the card was open isn't reverted. Tags go as add/remove sets for the same reason.
  mira_gui::GamePatch patch;
  const auto changed = [](const std::optional<std::string>& now, const std::optional<std::string>& before,
                          std::optional<std::string>& out) {
    if (now != before) out = now;
  };
  changed(current.name, original_patch_.name, patch.name);
  changed(current.exe_path, original_patch_.exe_path, patch.exe_path);
  changed(current.args, original_patch_.args, patch.args);
  changed(current.working_dir, original_patch_.working_dir, patch.working_dir);
  changed(current.runner_ref, original_patch_.runner_ref, patch.runner_ref);
  changed(current.data_dir, original_patch_.data_dir, patch.data_dir);
  changed(current.runner_config_json, original_patch_.runner_config_json, patch.runner_config_json);
  changed(current.env_json, original_patch_.env_json, patch.env_json);
  // mirad merges both objects, so only changed keys go, and a removed one as null.
  for (auto [now, before] : {std::pair{&patch.runner_config_json, &original_patch_.runner_config_json},
                             std::pair{&patch.env_json, &original_patch_.env_json}}) {
    if (!*now) continue;
    *now = mapping::MergePatchBetween(nlohmann::json::parse(before->value_or("{}"), nullptr, false),
                                      nlohmann::json::parse(**now, nullptr, false))
               .dump();
  }
  const bool fields_changed = patch.name || patch.exe_path || patch.args || patch.working_dir || patch.runner_ref ||
                              patch.data_dir || patch.runner_config_json || patch.env_json;

  const mira_gui::GamesPatch tags = TagsPatch(current.tags);
  const bool tags_changed = !tags.add_tags.empty() || !tags.remove_tags.empty() || tags.folder_tag.has_value();

  const auto fail = [this](const std::string& error) {
    setEnabled(true);
    UpdateModified();  // what didn't save still counts as a change
    emit SaveFinished(false, QString::fromStdString(error));
  };
  const auto succeed = [this, current] {
    setEnabled(true);
    UpdateModified();
    emit Loaded(QString::fromStdString(current.name.value_or(std::string())));
    emit SaveFinished(true, QString());
  };
  const auto save_overrides = [this, override_edits, fail, succeed] {
    if (override_edits.empty()) return succeed();
    mira_gui::api::PatchGameConfigAsync(
        this, id_, override_edits, [this, fail, succeed](mira_gui::PatchGameConfigResult override_result) {
          if (!override_result.ok) return fail(override_result.error);
          overrides_->MarkSaved();
          succeed();
        });
  };
  // Each part counts as saved only once its own request succeeds.
  const auto save_tags = [this, tags, tags_changed, current, fail, save_overrides] {
    const std::optional<std::vector<std::string>> saved_tags = original_patch_.tags;
    original_patch_ = current;
    original_patch_.tags = saved_tags;
    if (!tags_changed) {
      save_overrides();
      return;
    }
    mira_gui::api::PatchGamesAsync(
        this, tags, [this, tags, current, fail, save_overrides](mira_gui::PatchGamesResult result) {
          if (!result.ok) {
            fail(result.error);
            return;
          }
          original_patch_.tags = current.tags;
          if (tags.folder_tag) original_pick_ = *tags.folder_tag;
          save_overrides();
        });
  };

  setEnabled(false);
  if (!fields_changed) {
    save_tags();
    return;
  }
  mira_gui::api::PatchGameAsync(this, id_, patch, [fail, save_tags](mira_gui::PatchGameResult result) {
    if (!result.ok) {
      fail(result.error);
      return;
    }
    save_tags();
  });
}

}  // namespace mira_gui
