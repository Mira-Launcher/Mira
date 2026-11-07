#include "GameEditForm.h"

#include <QAbstractItemView>
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
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <filesystem>

#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../app/Shortcuts.h"
#include "../client/Async.h"
#include "../client/EventHub.h"
#include "../client/JsonMapping.h"
#include "../client/api/Config.h"
#include "../client/api/Emulators.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../client/api/Runners.h"
#include "../settings/SettingEditor.h"
#include "../settings/SettingsCard.h"
#include "../sources/Sources.h"
#include "../widgets/FlatCombo.h"
#include "../widgets/Labels.h"
#include "../widgets/PathField.h"
#include "../widgets/Scrolling.h"
#include "../widgets/TagEdit.h"
#include "HeroBackdrop.h"
#include "LaunchOptionsEdit.h"
#include "OverridesEditor.h"

namespace mira_gui {
namespace {

// The settings a store's card holds for its games, in this order, rather than Overrides.
std::vector<std::string> StoreKeys(const std::string& source) {
  if (source == "steam")
    return {"steam.launch_mode", "steam.launch_silent", "steam.track_process", "saves.skip_cloud"};
  if (source == "epic") return {"epic.eos_overlay", "saves.cloud"};
  if (source == "gog") return {"gog.comet", "saves.cloud"};
  return {};
}

QComboBox* EditableCombo(QWidget* parent, const QString& placeholder) {
  auto* combo = new QComboBox(parent);
  combo->setEditable(true);
  combo->setInsertPolicy(QComboBox::NoInsert);
  combo->lineEdit()->setPlaceholderText(placeholder);
  return combo;
}

// As wide as Settings' fields, so the two read alike.
QLineEdit* LineEdit(QWidget* parent, const QString& placeholder = {}) {
  auto* edit = new QLineEdit(parent);
  edit->setPlaceholderText(placeholder);
  edit->setFixedWidth(kFieldColumnWidth);
  return edit;
}

}  // namespace

GameEditForm::GameEditForm(std::string id, QWidget* parent) : QWidget(parent), id_(std::move(id)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setStyleSheet(
      "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
  scroll_->viewport()->setAutoFillBackground(false);
  SetUpScrolling(scroll_);
  auto* cards_page = new QWidget();
  cards_layout_ = new QVBoxLayout(cards_page);
  cards_layout_->setSpacing(16);
  scroll_->setWidget(cards_page);
  scroll_->viewport()->installEventFilter(this);
  FitColumn();
  layout->addWidget(scroll_);

  last_error_label_ = new QLabel(cards_page);
  last_error_label_->setWordWrap(true);
  last_error_label_->setProperty("role", "error");
  last_error_label_->hide();
  cards_layout_->addWidget(last_error_label_);

  // The rows are made here and put into cards by Arrange, once the game says which it has.
  exe_combo_ = EditableCombo(this, "Choose the program to run");
  connect(exe_combo_, &QComboBox::activated, this, &GameEditForm::OnExeComboActivated);
  // A long candidate keeps its file name in the list, and the field's tooltip is the whole path.
  exe_combo_->view()->setTextElideMode(Qt::ElideMiddle);
  exe_combo_->view()->setMinimumWidth(2 * kFieldColumnWidth);
  connect(exe_combo_, &QComboBox::editTextChanged, this, [this](const QString& text) {
    exe_combo_->setToolTip(
        text.isEmpty() || install_path_.empty()
            ? text
            : QString::fromStdString((std::filesystem::path(install_path_) / text.toStdString())
                                         .lexically_normal()
                                         .string()));
  });
  auto* browse = new QPushButton("Browse…", this);
  connect(browse, &QPushButton::clicked, this, &GameEditForm::BrowseExecutable);
  // Shown while mirad wants its pick confirmed; saving any change confirms it too.
  check_tag_ = MakeTag(this, "Not checked", "warning");
  check_tag_->setToolTip("Mira wasn't sure which program starts this game");
  check_tag_->hide();
  looks_right_ = new QPushButton("Looks right", this);
  looks_right_->setToolTip("Keep this executable and stop asking");
  looks_right_->hide();
  connect(looks_right_, &QPushButton::clicked, this, &GameEditForm::ConfirmExecutable);
  exe_row_ = new SettingRow("Executable", QString(), this);
  exe_row_->AddAfterLabel(check_tag_);
  exe_row_->AddControl(looks_right_);
  exe_row_->AddControl(FieldWithButtons(exe_combo_, {browse}));

  args_edit_ = LineEdit(this, "None");
  args_row_ = new SettingRow("Arguments", QString(), this);
  args_row_->AddControl(args_edit_);

  working_dir_edit_ = LineEdit(this, "The install folder");
  working_dir_row_ = new SettingRow("Working folder", QString(), this);
  working_dir_row_->AddControl(working_dir_edit_);

  launch_options_ = new LaunchOptionsEdit(this);
  launch_options_row_ = new SettingRow(
      "Launch options",
      "Other ways to start it, and programs to run beside it or open for it, such as mod managers.",
      this);
  launch_options_row_->AddControl(launch_options_->EmptyLabel());
  launch_options_row_->AddControl(launch_options_->AddButton());
  launch_options_row_->SetBelowFlush(launch_options_);
  connect(launch_options_, &LaunchOptionsEdit::Changed, this, &GameEditForm::UpdateModified);
  play_starts_row_ = new SettingRow("Play starts", QString(), this);
  play_starts_row_->AddControl(launch_options_->PlayStarts());
  only_choice_row_ = new SettingRow("Offer nothing else",
                                    "Play starts it without asking. Ctrl+click Play still shows "
                                    "every choice.",
                                    this);
  only_choice_row_->AddControl(launch_options_->OnlyChoice());

  runner_combo_ = new FlatCombo(this);
  runner_combo_->addItem("Default runner", QString());  // the runners come later
  runner_row_ = new SettingRow("Runner", QString(), this);
  runner_row_->AddControl(runner_combo_);
  runner_options_ = new ExpanderRow(QString(), this);  // titled and filled per runner kind
  runner_options_->hide();

  emulator_row_ = new SettingRow(
      "Emulator", "Picking another moves its files to that emulator's games folder.", this);
  emulator_combo_ = new FlatCombo(emulator_row_);
  emulator_row_->AddControl(emulator_combo_);
  connect(emulator_combo_, &QComboBox::activated, this, [this](int index) {
    if (index <= 0) return;
    const QString name = emulator_combo_->currentText();
    api::MoveGameToEmulatorAsync(this, id_, emulator_combo_->currentData().toString().toStdString(),
                                 [this, name](StoreActionResult moved) {
                                   if (!moved.ok) {
                                     emulator_combo_->setCurrentIndex(0);
                                     notify::FailedRequest(
                                         this, "Could not move it to " + name + ".", moved.error);
                                   }
                                 });
  });

  name_edit_ = LineEdit(this);
  name_row_ = new SettingRow("Name", QString(), this);
  name_row_->AddControl(name_edit_);

  tags_edit_ = new TagEdit(this);
  connect(tags_edit_, &TagEdit::TagClicked, this, &GameEditForm::TagFilterRequested);
  tags_row_ =
      new SettingRow("Tags",
                     "Labels of your choice. The \"hidden\" tag keeps this game out of the "
                     "library until you ask for it " +
                         shortcuts::HiddenGamesHint() +
                         ". In a library folder sorted by tag, the outlined tag is the folder "
                         "the game is in; drag another folder tag ahead of it to move the "
                         "game. Folder tags are chosen on the "
                         "Tags page.",
                     this);
  tags_row_->AddWideControl(tags_edit_);

  // Read-only: editing the path would only repoint the record, Move… moves the files too.
  install_path_edit_ = LineEdit(this, "Not installed");
  install_path_edit_->setReadOnly(true);
  open_install_ = new QPushButton("Open", this);
  open_install_->setToolTip("Open this folder in the file manager");
  connect(open_install_, &QPushButton::clicked, this, [this] {
    std::filesystem::path folder(install_path_);
    // An emulator's game is a file; its folder is what opens.
    if (emulator_row_->isVisibleTo(this)) folder = folder.parent_path();
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder.string())));
  });
  move_button_ = new QPushButton("Move…", this);
  connect(move_button_, &QPushButton::clicked, this, [this] { MoveFolder(/*prefix=*/false); });
  install_row_ = new SettingRow("Install folder", QString(), this);
  install_row_->AddControl(FieldWithButtons(install_path_edit_, {open_install_, move_button_}));

  data_dir_edit_ = LineEdit(this, "Automatic");
  open_data_dir_ = new QPushButton("Open", this);
  open_data_dir_->setToolTip("Open this folder in the file manager");
  open_data_dir_->setEnabled(false);
  connect(open_data_dir_, &QPushButton::clicked, this,
          [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(data_dir_edit_->text())); });
  // Editing the path repoints the record to another prefix; Move… moves this one.
  move_prefix_ = new QPushButton("Move…", this);
  move_prefix_->setVisible(false);
  connect(move_prefix_, &QPushButton::clicked, this, [this] { MoveFolder(/*prefix=*/true); });
  data_dir_row_ = new SettingRow("Prefix and data folder",
                                 "The folder holding this game's Wine or Proton prefix and its "
                                 "data.",
                                 this);
  data_dir_row_->AddControl(FieldWithButtons(data_dir_edit_, {open_data_dir_, move_prefix_}));

  for (SettingRow* row : {exe_row_, args_row_, working_dir_row_, launch_options_row_,
                          play_starts_row_, only_choice_row_, runner_row_, emulator_row_, name_row_,
                          tags_row_, install_row_, data_dir_row_})
    row->hide();  // until Arrange puts it in a card

  overrides_ = new mira_gui::OverridesEditor(id_, this);
  connect(overrides_, &OverridesEditor::Changed, this, [this] {
    // Steam passes the environment on only to a game it doesn't start itself.
    if (source_ == "steam")
      overrides_->SetRowHidden("launch.env", overrides_->Text("steam.launch_mode") != "direct");
    emit Changed();
  });
  connect(overrides_, &OverridesEditor::RevealRequested, this, [this](QWidget* row) {
    // After the expander's layout has opened.
    QTimer::singleShot(0, row, [this, row] { scroll_->ensureWidgetVisible(row, 0, 48); });
  });

  for (QLineEdit* edit : {name_edit_, args_edit_, working_dir_edit_, data_dir_edit_}) {
    connect(edit, &QLineEdit::textChanged, this, &GameEditForm::UpdateModified);
  }
  connect(exe_combo_, &QComboBox::editTextChanged, this, &GameEditForm::UpdateModified);
  connect(runner_combo_, &QComboBox::currentIndexChanged, this, &GameEditForm::UpdateModified);
  connect(runner_combo_, &QComboBox::currentIndexChanged, this, &GameEditForm::ShowRunnerOptions);
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
  // The launch record's parts each have a row: the list, which one Play starts, and whether it's
  // the only choice.
  const auto revert_launch = [this](SettingRow* row, auto part) {
    connect(row, &SettingRow::RevertClicked, this, [this, part] {
      mira_gui::GamePatch patch = CurrentPatch();
      GameLaunch now = patch.launch.value_or(GameLaunch());
      part(now, original_patch_.launch.value_or(GameLaunch()));
      patch.launch = now;
      populating_ = true;
      ShowPatch(patch);
      populating_ = false;
      UpdateModified();
    });
  };
  revert_launch(launch_options_row_,
                [](GameLaunch& now, const GameLaunch& saved) { now.options = saved.options; });
  revert_launch(play_starts_row_, [](GameLaunch& now, const GameLaunch& saved) {
    now.default_id = saved.default_id;
    now.only = saved.only && !saved.default_id.empty();
  });
  revert_launch(only_choice_row_, [](GameLaunch& now, const GameLaunch& saved) {
    now.only = saved.only && !now.default_id.empty();
  });
  revert(tags_row_, &mira_gui::GamePatch::tags);
  revert(runner_row_, &mira_gui::GamePatch::runner_ref);
  revert(data_dir_row_, &mira_gui::GamePatch::data_dir);
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

void GameEditForm::SetTagSuggestions(const QStringList& tags) {
  tags_edit_->SetSuggestions(tags);
}

void GameEditForm::SetTagOrder(std::vector<std::string> order) {
  tags_edit_->SetOrder(std::move(order));
}

void GameEditForm::SetBottomRoom(int height) {
  bottom_room_ = height;
  FitColumn();
}

void GameEditForm::SetColumnWidth(int width) {
  column_width_ = width;
  FitColumn();
}

bool GameEditForm::eventFilter(QObject* watched, QEvent* event) {
  if (watched == scroll_->viewport() && event->type() == QEvent::Resize) FitColumn();
  return QWidget::eventFilter(watched, event);
}

void GameEditForm::FitColumn() {
  const int room = scroll_->viewport()->width();
  const int side =
      column_width_ > 0 ? std::max(kCardInset, (room - column_width_) / 2) : kCardInset;
  cards_layout_->setContentsMargins(side, 18, side, 28 + bottom_room_);
}

void GameEditForm::RevealSetting(const std::string& key) {
  overrides_->RevealKey(key);
}

void GameEditForm::Arrange(const mira_gui::GameDetail& game) {
  arranged_ = true;
  QWidget* page = scroll_->widget();
  const auto add_card = [this, page](const QString& title, std::initializer_list<QWidget*> rows) {
    auto* card = new SettingsCard(title, page);
    for (QWidget* row : rows) {
      card->AddRow(row);
      row->show();
    }
    cards_layout_->addWidget(card);
    return card;
  };
  const bool emulated = game.runner_ref.starts_with("emulator:");
  const std::vector<std::string> store_keys = StoreKeys(game.source);
  const auto place_store_keys = [this, &store_keys](SettingsCard* card) {
    for (size_t i = 0; i < store_keys.size(); ++i)
      overrides_->PlaceRow(store_keys[i], card, static_cast<int>(i));
  };
  // The scripts, each with its ready-made steps inside its row, for any game that can use them.
  const auto add_scripts = [this, &add_card] {
    SettingsCard* scripts = add_card("Scripts", {});
    for (const char* key : {"launch.pre_script", "launch.post_script"})
      overrides_->PlaceRow(key, scripts);
    overrides_->HideWhenEmpty(scripts);
  };

  add_card("Details", {name_row_, tags_row_});
  if (game.source == "steam") {
    // Steam starts the game from its own record, so there is no executable or runner to pick.
    SettingsCard* steam =
        add_card("Steam", {launch_options_row_, play_starts_row_, only_choice_row_});
    place_store_keys(steam);
    overrides_->PlaceRow("launch.env", steam);
    add_scripts();
    add_card("Files", {install_row_, data_dir_row_});
  } else if (emulated) {
    SettingsCard* emulator = add_card("Emulator", {emulator_row_, args_row_});
    overrides_->PlaceRow("launch.env", emulator);
    add_scripts();
    install_row_->Label()->setText("Game file");
    add_card("Files", {install_row_});
  } else {
    add_card("Launch", {exe_row_, args_row_, working_dir_row_, launch_options_row_,
                        play_starts_row_, only_choice_row_});
    SettingsCard* runner = add_card("Runner", {runner_row_, runner_options_});
    runner_options_->hide();  // until the runner kind has options
    overrides_->PlaceRow("launch.env", runner);
    if (!store_keys.empty()) {
      const SourceInfo* store = FindSourceInfo(QString::fromStdString(game.source));
      SettingsCard* card =
          add_card(store != nullptr ? store->name : QString::fromStdString(game.source), {});
      place_store_keys(card);
      overrides_->HideWhenEmpty(card);
    }
    add_scripts();
    add_card("Files", {install_row_, data_dir_row_});
  }
  auto* overrides = new SettingsCard("Overrides", page);
  cards_layout_->addWidget(overrides);
  overrides_->SetOverridesCard(overrides);
  cards_layout_->addStretch(1);
}

void GameEditForm::ResetScroll() {
  scroll_->verticalScrollBar()->setValue(0);
}

void GameEditForm::Reload() {
  if (IsDirty()) {
    stale_ = true;
  } else {
    Load();
  }
}

void GameEditForm::Load() {
  stale_ = false;
  mira_gui::api::ListRunnersAsync(
      this, [this](mira_gui::RunnersResult result) { PopulateRunnerCombo(result); });
  mira_gui::api::GetGameAsync(this, id_, [this](mira_gui::GameDetailResult result) {
    if (!result.ok) {
      emit LoadFailed(error_help::Describe(result.error), !loaded_);
      return;
    }
    if (loaded_ && IsDirty()) {  // keep the typing; Discard loads it again
      stale_ = true;
      return;
    }
    setEnabled(true);
    if (!arranged_) Arrange(result.game);
    Populate(result.game);
    overrides_->Load();  // after Arrange said where its rows go
  });
}

void GameEditForm::Populate(const mira_gui::GameDetail& game) {
  loaded_ = true;
  emit Loaded(QString::fromStdString(game.name));

  install_path_ = game.install_path;
  source_ = game.source;
  windows_ = game.platform == "windows";
  if (web_ != (game.platform == "web")) {
    web_ = game.platform == "web";
    PopulateRunnerCombo(runners_);
  }
  noun_ = std::ranges::contains(game.tags, tags::kApp) ? "app" : "game";
  move_button_->setToolTip("Move this " + noun_ + "'s files to another folder");
  move_prefix_->setToolTip("Move this " + noun_ + "'s prefix to another folder");
  ShowInstallPath();
  // A desktop entry's files belong to another app; Steam moves its own games.
  move_button_->setVisible(!game.install_path.empty() && game.source != "desktop-entry" &&
                           game.source != "steam");
  open_install_->setVisible(!game.install_path.empty());

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
  patch.launch = game.launch;
  default_runner_ = game.default_runner;
  tags_edit_->SetFolderTags(game.folder_tags);
  original_pick_ = game.folder_tag;
  ShowPatch(patch);
  tags_edit_->SetFolderPick(original_pick_);
  populating_ = false;

  original_patch_ = CurrentPatch();
  // A native game or a desktop entry runs without a prefix (a native game keeps its old one only
  // for turning back into a Windows game); an HTML game's data folder holds its browser and saves,
  // which Mira manages.
  const bool own_files = game.source != "desktop-entry" && game.platform != "web";
  data_dir_row_->setVisible(data_dir_row_->parentWidget() != this && own_files &&
                            game.platform != "native" &&
                            (game.source != "steam" || !game.data_dir.empty()));
  move_prefix_->setVisible(own_files && !game.data_dir.empty() && game.source != "steam");
  // An emulator's game names its emulator, and picking another moves it there.
  if (game.runner_ref.starts_with("emulator:")) {
    // The file the emulator opens, rather than the folder it is in.
    if (!game.exe_path.empty()) {
      install_path_edit_->setText(QString::fromStdString(
          (std::filesystem::path(install_path_) / game.exe_path).lexically_normal().string()));
      ShowPathStart(install_path_edit_);
      install_path_edit_->setToolTip(install_path_edit_->text());
    }
    const SourceInfo* source = FindSourceInfo(QString::fromStdString(game.source));
    emulator_combo_->clear();
    emulator_combo_->addItem(source != nullptr ? source->name : QString::fromStdString(game.source),
                             QString::fromStdString(game.source));
    emulator_combo_->setEnabled(false);
    emulator_combo_->setToolTip(QString());
    api::GetGameEmulatorsAsync(
        emulator_combo_, game.id, [this, id = game.id](EmulatorChoicesResult result) {
          if (!result.ok || id != id_) return;
          while (emulator_combo_->count() > 1)
            emulator_combo_->removeItem(1);  // an earlier Populate's answer
          for (const EmulatorChoice& choice : result.emulators) {
            emulator_combo_->addItem(QString::fromStdString(choice.name),
                                     QString::fromStdString(choice.id));
          }
          emulator_combo_->setEnabled(!result.emulators.empty());
          if (result.emulators.empty())
            emulator_combo_->setToolTip("No other installed emulator plays this");
        });
  }
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
  for (QLineEdit* edit : {working_dir_edit_, data_dir_edit_, exe_combo_->lineEdit()})
    ShowPathStart(edit);
  SelectOrKeep(runner_combo_, QString::fromStdString(patch.runner_ref.value_or(std::string())));
  tags_edit_->SetTags(patch.tags.value_or(std::vector<std::string>()));
  runner_config_ = nlohmann::json::parse(patch.runner_config_json.value_or("{}"), nullptr, false);
  if (!runner_config_.is_object()) runner_config_ = nlohmann::json::object();
  ShowRunnerFieldValues();
  const SourceInfo* store = FindSourceInfo(QString::fromStdString(source_));
  launch_options_->SetLaunch(patch.launch.value_or(GameLaunch()), windows_, install_path_,
                             store != nullptr && store->signs_in ? store->name : QString());
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
  if (const auto known = runner_schemas_.find(kind);
      known != runner_schemas_.end() || kind.empty()) {
    BuildRunnerFields(kind.empty() ? std::vector<RunnerOption>() : known->second);
    return;
  }
  api::GetRunnerSchemaAsync(this, kind, [this, kind](RunnerSchemaResult result) {
    // An unknown kind (a typed-in reference) simply has no options to show. Only an answer is
    // kept, so a failed request is made again next time.
    if (result.ok) runner_schemas_[kind] = result.options;
    if (kind == options_kind_)
      BuildRunnerFields(result.ok ? result.options : std::vector<RunnerOption>());
  });
}

void GameEditForm::BuildRunnerFields(const std::vector<RunnerOption>& options) {
  runner_options_->ClearRows();
  runner_fields_.clear();
  QString title = QString::fromStdString(options_kind_);
  if (!title.isEmpty()) title[0] = title[0].toUpper();
  runner_options_->Label()->setText(title + " options");
  for (const RunnerOption& option : options) {
    auto* row = new SettingRow(QString::fromStdString(option.label),
                               QString::fromStdString(option.doc), runner_options_);
    QLineEdit* edit = LineEdit(row, "Not set");
    row->AddControl(edit);
    runner_options_->AddRow(row);
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
      const nlohmann::json saved =
          nlohmann::json::parse(original_patch_.runner_config_json.value_or("{}"), nullptr, false);
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
  runner_options_->setVisible(!options.empty() && runner_row_->parentWidget() != this);
  ShowRunnerFieldValues();
  UpdateModified();
}

void GameEditForm::ShowRunnerFieldValues() {
  int set = 0;
  for (const RunnerField& field : runner_fields_) {
    const auto it = runner_config_.find(field.key);
    field.edit->setText(it == runner_config_.end() ? QString()
                        : it->is_string()          ? QString::fromStdString(it->get<std::string>())
                                                   : QString::fromStdString(it->dump()));
    field.edit->setCursorPosition(0);
    set += it != runner_config_.end();
  }
  runner_options_->SetSummary(set == 0 ? QString("Not set") : QString("%1 set").arg(set));
}

void GameEditForm::ShowInstallPath() {
  install_path_edit_->setText(QString::fromStdString(install_path_));
  ShowPathStart(install_path_edit_);
  install_path_edit_->setToolTip(install_path_edit_->text());
}

void GameEditForm::PopulateExeCombo(const std::vector<mira_gui::GameDetail::Candidate>& candidates,
                                    const std::string& current) {
  std::vector<mira_gui::GameDetail::Candidate> sorted = candidates;
  std::ranges::sort(sorted, std::greater{}, &mira_gui::GameDetail::Candidate::score);

  exe_combo_->blockSignals(true);
  exe_combo_->clear();
  for (const mira_gui::GameDetail::Candidate& candidate : sorted) {
    QString label =
        QString("%1 (%2, score %3)")
            .arg(QString::fromStdString(candidate.rel_path), QString::fromStdString(candidate.kind))
            .arg(candidate.score, 0, 'f', 1);
    if (candidate.is_installer) label += " (installer, probably not the game)";
    exe_combo_->addItem(label, QString::fromStdString(candidate.rel_path));
  }
  exe_combo_->setEditText(QString::fromStdString(current));
  ShowPathStart(exe_combo_->lineEdit());
  exe_combo_->blockSignals(false);
}

void GameEditForm::PopulateRunnerCombo(const mira_gui::RunnersResult& result) {
  runners_ = result;
  const QString current = RunnerRef(runner_combo_);
  runner_combo_->blockSignals(true);
  runner_combo_->clear();
  runner_combo_->addItem(web_ ? "Default browser" : "Default runner", QString());
  if (result.ok) {
    for (const mira_gui::RunnerInfo& runner : result.runners) {
      if ((runner.kind == "browser") != web_) continue;
      const QString label = QString("%1 (%2)").arg(QString::fromStdString(runner.name),
                                                   QString::fromStdString(runner.kind));
      runner_combo_->addItem(label, QString::fromStdString(runner.reference));
    }
  }
  SelectOrKeep(runner_combo_, current);
  runner_combo_->blockSignals(false);
}

void GameEditForm::OnExeComboActivated(int index) {
  exe_combo_->setEditText(exe_combo_->itemData(index).toString());
  ShowPathStart(exe_combo_->lineEdit());
}

void GameEditForm::BrowseExecutable() {
  const QString start_dir =
      install_path_.empty() ? QString() : QString::fromStdString(install_path_);
  const QString selected = QFileDialog::getOpenFileName(this, "Select executable", start_dir);
  if (selected.isEmpty()) return;

  exe_combo_->setEditText(RelativeIfInside(selected, install_path_));
  ShowPathStart(exe_combo_->lineEdit());
}

void GameEditForm::MoveFolder(bool prefix) {
  const std::string current =
      prefix ? original_patch_.data_dir.value_or(std::string()) : install_path_;
  const QString parent_dir =
      current.empty()
          ? QString()
          : QString::fromStdString(std::filesystem::path(current).parent_path().string());
  const QString picked = QFileDialog::getExistingDirectory(
      this, prefix ? "Move the prefix folder into" : "Move the " + noun_ + "'s folder into",
      parent_dir);
  if (picked.isEmpty()) return;
  const std::string target =
      (std::filesystem::path(picked.toStdString()) / std::filesystem::path(current).filename())
          .string();
  if (target == current) return;
  const QString what = "this " + noun_ + (prefix ? "'s prefix" : "'s files");
  if (!notify::Confirm(
          this, prefix ? "Move prefix" : "Move " + noun_,
          QString(
              "Move %1 to %2? A move to another drive copies them first, which can take a while.")
              .arg(what, QString::fromStdString(target)),
          "Move")) {
    return;
  }
  QPushButton* button = prefix ? move_prefix_ : move_button_;
  button->setEnabled(false);
  button->setText("Moving…");
  api::RelocateGameAsync(
      this, id_, prefix ? std::string() : target, prefix ? target : std::string(),
      [this, button, prefix](GameDetailResult result) {
        button->setText("Move…");
        if (!result.ok) {
          button->setEnabled(true);
          notify::FailedRequest(
              this, prefix ? "Could not move the prefix." : "Could not move the " + noun_ + ".",
              result.error);
          return;
        }
        install_path_ = result.game.install_path;
        ShowInstallPath();
        // Moved on disk and saved, so not an unsaved change.
        original_patch_.data_dir = result.game.data_dir;
        populating_ = true;
        data_dir_edit_->setText(QString::fromStdString(result.game.data_dir));
        ShowPathStart(data_dir_edit_);
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
  patch.launch = launch_options_->Launch();
  return patch;
}

void GameEditForm::UpdateModified() {
  if (populating_) return;
  const mira_gui::GamePatch now = CurrentPatch();
  name_row_->SetModified(now.name != original_patch_.name);
  exe_row_->SetModified(now.exe_path != original_patch_.exe_path);
  args_row_->SetModified(now.args != original_patch_.args);
  working_dir_row_->SetModified(now.working_dir != original_patch_.working_dir);
  const GameLaunch launch = now.launch.value_or(GameLaunch());
  const GameLaunch saved_launch = original_patch_.launch.value_or(GameLaunch());
  launch_options_row_->SetModified(launch.options != saved_launch.options);
  play_starts_row_->SetModified(launch.default_id != saved_launch.default_id);
  only_choice_row_->SetModified(launch.only != saved_launch.only);
  tags_row_->SetModified(now.tags != original_patch_.tags ||
                         tags_edit_->FolderPick() != original_pick_);
  runner_row_->SetModified(now.runner_ref != original_patch_.runner_ref);
  data_dir_row_->SetModified(now.data_dir != original_patch_.data_dir);
  move_prefix_->setEnabled(now.data_dir == original_patch_.data_dir);  // moves the saved prefix
  const nlohmann::json saved_config =
      nlohmann::json::parse(original_patch_.runner_config_json.value_or("{}"), nullptr, false);
  for (const RunnerField& field : runner_fields_) {
    const nlohmann::json was = saved_config.is_object()
                                   ? saved_config.value(field.key, nlohmann::json())
                                   : nlohmann::json();
    field.row->SetModified(runner_config_.value(field.key, nlohmann::json()) != was);
  }
  emit Changed();
}

int GameEditForm::ChangeCount() const {
  const mira_gui::GamePatch now = CurrentPatch();
  const int fields =
      (now.name != original_patch_.name) + (now.exe_path != original_patch_.exe_path) +
      (now.args != original_patch_.args) + (now.working_dir != original_patch_.working_dir) +
      (now.tags != original_patch_.tags || tags_edit_->FolderPick() != original_pick_) +
      (now.runner_ref != original_patch_.runner_ref) + (now.data_dir != original_patch_.data_dir) +
      (now.launch != original_patch_.launch);
  // One per runner option, since each is its own row.
  const int runner_options = static_cast<int>(
      mapping::MergePatchBetween(
          nlohmann::json::parse(original_patch_.runner_config_json.value_or("{}"), nullptr, false),
          runner_config_)
          .size());
  return fields + runner_options + static_cast<int>(overrides_->PendingEdits().size());
}

bool GameEditForm::IsDirty() const {
  return ChangeCount() > 0;
}

void GameEditForm::DiscardChanges() {
  populating_ = true;
  ShowPatch(original_patch_);
  tags_edit_->SetFolderPick(original_pick_);
  populating_ = false;
  overrides_->DiscardChanges();
  UpdateModified();
  if (stale_) Load();
}

mira_gui::GamesPatch GameEditForm::TagsPatch(
    const std::optional<std::vector<std::string>>& now) const {
  mira_gui::GamesPatch tags;
  tags.ids = {id_};
  const std::vector<std::string> now_tags = now.value_or(std::vector<std::string>());
  const std::vector<std::string> was_tags =
      original_patch_.tags.value_or(std::vector<std::string>());
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
  const auto changed = [](const std::optional<std::string>& now,
                          const std::optional<std::string>& before,
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
  if (current.launch != original_patch_.launch) patch.launch = current.launch;
  // mirad merges the object, so only changed keys go, and a removed one as null.
  if (patch.runner_config_json) {
    patch.runner_config_json =
        mapping::MergePatchBetween(
            nlohmann::json::parse(original_patch_.runner_config_json.value_or("{}"), nullptr,
                                  false),
            nlohmann::json::parse(*patch.runner_config_json, nullptr, false))
            .dump();
  }
  const bool fields_changed = patch.name || patch.exe_path || patch.args || patch.working_dir ||
                              patch.runner_ref || patch.data_dir || patch.runner_config_json ||
                              patch.launch;

  const mira_gui::GamesPatch tags = TagsPatch(current.tags);
  const bool tags_changed =
      !tags.add_tags.empty() || !tags.remove_tags.empty() || tags.folder_tag.has_value();

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
        this, id_, override_edits,
        [this, fail, succeed](mira_gui::PatchGameConfigResult override_result) {
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
  mira_gui::api::PatchGameAsync(this, id_, patch,
                                [fail, save_tags](mira_gui::PatchGameResult result) {
                                  if (!result.ok) {
                                    fail(result.error);
                                    return;
                                  }
                                  save_tags();
                                });
}

}  // namespace mira_gui
