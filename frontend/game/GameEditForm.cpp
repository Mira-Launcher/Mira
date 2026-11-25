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
#include <map>

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
#include "../components/Badge.h"
#include "../components/Card.h"
#include "../components/ColumnGrid.h"
#include "../components/FlatCombo.h"
#include "../components/Labels.h"
#include "../components/Metrics.h"
#include "../components/PathField.h"
#include "../components/Scrolling.h"
#include "../components/SettingRow.h"
#include "../components/Switch.h"
#include "../components/Text.h"
#include "../settings/SettingEditor.h"
#include "../sources/Sources.h"
#include "CardColumns.h"
#include "HeroBackdrop.h"
#include "LaunchOptionsEdit.h"
#include "OverridesEditor.h"
#include "TagEdit.h"

namespace mira_gui {
namespace {

// A read-only path field shows its start, with the whole path as its tooltip.
void ShowReadOnlyPath(QLineEdit* edit, const std::string& path) {
  edit->setText(QString::fromStdString(path));
  ShowPathStart(edit);
  edit->setToolTip(edit->text());
}

// "Age of Wonders · GOG 1207658883 · Steam 61500, from the umu database".
QString IdentityText(const mira_gui::GameDetail& game) {
  const auto& identity = game.identity;
  static const std::map<std::string, QString> kStores = {
      {"steam", "Steam"},     {"gog", "GOG"},       {"egs", "Epic"},
      {"amazon", "Amazon"},   {"humble", "Humble"}, {"itchio", "itch.io"},
      {"ubisoft", "Ubisoft"}, {"ea", "EA"},         {"battlenet", "Battle.net"}};
  static const std::map<std::string, QString> kFrom = {{"override", "set below"},
                                                       {"file", "from its steam_appid.txt"},
                                                       {"umu", "from the umu database"},
                                                       {"name", "by its name"}};
  QStringList parts;
  if (!identity.title.empty()) parts << QString::fromStdString(identity.title);
  if (const auto store = kStores.find(identity.store);
      store != kStores.end() && identity.store != "steam" && !identity.store_id.empty())
    parts << store->second + " " + QString::fromStdString(identity.store_id);
  if (!identity.steam_appid.empty()) {
    QString steam = "Steam " + QString::fromStdString(identity.steam_appid);
    if (const auto from = kFrom.find(identity.steam_from); from != kFrom.end())
      steam += ", " + from->second;
    parts << steam;
  }
  return parts.isEmpty() ? QString("Not known yet. Its art and details may still match by name.")
                         : parts.join(" · ");
}

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
  SeeThrough(scroll_);
  SetUpScrolling(scroll_);
  auto* cards_page = new QWidget();
  cards_layout_ = new QVBoxLayout(cards_page);
  grid_ = new ColumnGrid(nullptr, /*max_columns=*/2);
  cards_layout_->addLayout(grid_);
  cards_layout_->addStretch(1);
  scroll_->setWidget(cards_page);
  FitColumn();
  layout->addWidget(scroll_);

  last_error_label_ = new QLabel(cards_page);
  last_error_label_->setWordWrap(true);
  SetTextStyle(last_error_label_, text::kError);
  last_error_label_->hide();
  grid_->AddHeader(last_error_label_);

  missing_ = new QWidget(cards_page);
  auto* missing_layout = new QHBoxLayout(missing_);
  missing_layout->setContentsMargins(0, 0, 0, 0);
  missing_text_ = new QLabel(missing_);
  missing_text_->setWordWrap(true);
  SetTextStyle(missing_text_, text::kError);
  missing_layout->addWidget(missing_text_, 1);
  auto* find = new QPushButton("Change folder…", missing_);
  connect(find, &QPushButton::clicked, this, [this] { ChangeFolder(/*prefix=*/false); });
  missing_layout->addWidget(find);
  missing_->hide();
  grid_->AddHeader(missing_);

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
  exe_row_ = new SettingRow("Executable", QString(), this);
  detect_ = new QPushButton("Detect again", this);
  detect_->setToolTip("Look through the game's folder again for the program that starts it");
  connect(detect_, &QPushButton::clicked, this, &GameEditForm::DetectExecutable);
  exe_row_->AddControlBelow(FieldWithButtons(exe_combo_, {browse, detect_}));

  args_edit_ = LineEdit(this, "None");
  args_row_ = new SettingRow("Arguments", QString(), this);
  args_row_->AddControl(args_edit_);

  working_dir_edit_ = LineEdit(this, "The game folder");
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
  only_choice_row_ = new SettingRow("Don't ask which to start",
                                    "Play starts the option above without asking. Ctrl+click Play "
                                    "still shows every choice.",
                                    this);
  only_choice_row_->AddControl(launch_options_->OnlyChoice());

  runner_combo_ = new FlatCombo(this);
  runner_combo_->addItem("Default runner", QString());  // the runners come later
  runner_row_ = new SettingRow("Runner", QString(), this);
  runner_row_->AddControl(runner_combo_);
  runner_options_ = new ExpanderRow(QString(), this);  // titled and filled per runner kind
  runner_options_->hide();
  identity_row_ = new SettingRow(
      "Identified as",
      "The game Mira takes this for. Its art, ratings, saves and Proton fixes all follow it; set "
      "its Steam game ID below when it's wrong.",
      this);
  identity_text_ = new QLabel(identity_row_);
  SetTextStyle(identity_text_, text::kSubtle);
  identity_text_->setWordWrap(true);
  identity_row_->AddControl(identity_text_, 1);
  identity_row_->hide();
  fix_row_ = new SettingRow("Game fix",
                            "Proton's protonfixes for this game, found by its umu game ID", this);
  fix_text_ = new QLabel(fix_row_);
  SetTextStyle(fix_text_, text::kSubtle);
  fix_row_->AddControl(fix_text_);
  fix_use_ = new QPushButton(fix_row_);
  fix_row_->AddControl(fix_use_);
  connect(fix_use_, &QPushButton::clicked, this, [this] {
    if (fix_known_)
      SelectOrKeep(runner_combo_, QString::fromStdString(fix_build_));
    else
      RevealSetting("metadata.steam_appid");
  });
  fix_row_->hide();

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

  // Read-only: Change… points the game at where its files are now; moving them is the menu's
  // Move to Mira's folders.
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
  change_install_ = new QPushButton("Change…", this);
  connect(change_install_, &QPushButton::clicked, this, [this] { ChangeFolder(/*prefix=*/false); });
  install_row_ = new SettingRow("Game folder", "Where the game's files are.", this);
  install_row_->AddControlBelow(
      FieldWithButtons(install_path_edit_, {open_install_, change_install_}));

  data_dir_edit_ = LineEdit(this, "Not made yet");
  data_dir_edit_->setReadOnly(true);
  open_data_dir_ = new QPushButton("Open", this);
  open_data_dir_->setToolTip("Open this folder in the file manager");
  open_data_dir_->setEnabled(false);
  connect(open_data_dir_, &QPushButton::clicked, this,
          [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(data_dir_edit_->text())); });
  change_data_dir_ = new QPushButton("Change…", this);
  change_data_dir_->setVisible(false);
  connect(change_data_dir_, &QPushButton::clicked, this, [this] { ChangeFolder(/*prefix=*/true); });
  data_dir_row_ = new SettingRow("Prefix folder",
                                 "The folder holding this game's Wine or Proton prefix.", this);
  data_dir_row_->AddControlBelow(
      FieldWithButtons(data_dir_edit_, {open_data_dir_, change_data_dir_}));

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
    // After the rows that just showed have their place.
    QTimer::singleShot(0, row, [this, row] {
      scroll_->ensureWidgetVisible(row, 0, 48);
      // Into its field, so the reveal shows even when the row was already in view.
      for (QWidget* child : row->findChildren<QWidget*>()) {
        if ((child->focusPolicy() & Qt::TabFocus) && child->isVisibleTo(row)) {
          child->setFocus(Qt::OtherFocusReason);
          break;
        }
      }
    });
  });
  connect(overrides_, &OverridesEditor::CardsChanged, this, &GameEditForm::CardsChanged);

  for (QLineEdit* edit : {name_edit_, args_edit_, working_dir_edit_}) {
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
  grid_->SetColumnWidth(width);
}

void GameEditForm::FitColumn() {
  cards_layout_->setContentsMargins(kCardInset, kCardInset, kCardInset, 28 + bottom_room_);
}

Card* GameEditForm::AddCard(const QString& title) {
  auto* card = new Card(title, scroll_->widget());
  grid_->addWidget(card);
  if (const auto it = std::ranges::find(runs_cards_, title); it != runs_cards_.end())
    grid_->Place(card, 2, 0);
  else  // the game's own cards, and any group CardColumns doesn't list
    grid_->Place(card, 2, 1);
  cards_.push_back(card);
  return card;
}

std::vector<GameEditForm::JumpGroup> GameEditForm::Jumps() const {
  JumpGroup runs{"How it runs", {}};
  JumpGroup game{"The game", {}};
  const auto collect = [this](const std::vector<QString>& titles, JumpGroup& group) {
    for (const QString& title : titles)
      for (Card* card : cards_)
        if (card->Title() == title) group.cards.push_back(card);
  };
  collect(runs_cards_, runs);
  collect(game_cards_, game);
  // A group CardColumns doesn't list.
  for (Card* card : cards_) {
    if (!std::ranges::contains(runs_cards_, card->Title()) &&
        !std::ranges::contains(game_cards_, card->Title()))
      game.cards.push_back(card);
  }
  return {runs, game};
}

void GameEditForm::RevealSetting(const std::string& key) {
  overrides_->RevealKey(key);
}

void GameEditForm::Arrange(const mira_gui::GameDetail& game) {
  arranged_ = true;
  const std::vector<std::string> store_keys = StoreKeys(game.source);
  const SourceInfo* store_info = FindSourceInfo(QString::fromStdString(game.source));
  const QString store = store_keys.empty() || game.source == "steam" ? QString()
                        : store_info != nullptr ? store_info->name
                                                : QString::fromStdString(game.source);
  const CardColumns columns = CardColumnsFor(game, store);
  runs_cards_ = columns.runs;
  game_cards_ = columns.game;
  const auto add_card = [this](const QString& title, std::initializer_list<QWidget*> rows) {
    Card* card = AddCard(title);
    for (QWidget* row : rows) {
      card->AddRow(row);
      row->show();
    }
    return card;
  };
  const bool emulated = game.runner_ref.starts_with("emulator:");
  const auto place_store_keys = [this, &store_keys](Card* card) {
    for (size_t i = 0; i < store_keys.size(); ++i)
      overrides_->PlaceRow(store_keys[i], card, static_cast<int>(i));
  };

  add_card("Details", {name_row_, tags_row_});
  // Everything that says which game it is, for its art, store info and saves. A Steam game is
  // its own, and an emulator's isn't a Steam or store game.
  Card* identity = add_card("Game detection", {});
  int next = 0;
  if (game.source != "steam" && !emulated) {
    identity->AddRow(identity_row_);
    identity_row_->show();
    overrides_->PlaceRow("metadata.steam_appid", identity, ++next);
  }
  overrides_->PlaceRow("metadata.steamgriddb_id", identity, next + 1);
  overrides_->PlaceRow("saves.manifest_name", identity);  // after them, though it loads first
  overrides_->HideWhenEmpty(identity);
  if (game.source == "steam") {
    // Steam starts the game from its own record, so there is no executable or runner to pick.
    Card* steam = add_card("Steam", {launch_options_row_, play_starts_row_, only_choice_row_});
    place_store_keys(steam);
    overrides_->PlaceGroup("Environment", steam);
  } else if (emulated) {
    Card* emulator = add_card("Emulator", {emulator_row_, args_row_});
    overrides_->PlaceGroup("Environment", emulator);
    install_row_->Label()->setText("Game file");
  } else {
    Card* launch = add_card("Launch", {exe_row_, args_row_, working_dir_row_, launch_options_row_,
                                       play_starts_row_, only_choice_row_});
    overrides_->PlaceGroup("Environment", launch);
    Card* runner = add_card("Runner", {runner_row_, fix_row_, runner_options_});
    runner_options_->hide();  // until the runner kind has options
    ShowFixStatus();
    overrides_->PlaceGroup("Proton", runner);
    if (!store.isEmpty()) {
      Card* card = add_card(store, {});
      place_store_keys(card);
      overrides_->HideWhenEmpty(card);
    }
  }
  Card* saves = add_card("Saves", {});
  overrides_->PlaceGroup("Backups", saves);
  overrides_->PlaceGroup("Cloud saves", saves);
  overrides_->PlaceGroup("Where saves are", saves);
  overrides_->HideWhenEmpty(saves);
  // The scripts, each with its ready-made steps inside its row, for any game that can use them.
  Card* scripts = add_card("Scripts", {});
  overrides_->PlaceGroup("Scripts", scripts);
  overrides_->HideWhenEmpty(scripts);
  if (emulated)
    add_card("Files", {install_row_});
  else
    add_card("Files", {install_row_, data_dir_row_});
  // Every other group of settings the game can use is a card of its own.
  overrides_->SetCardMaker([this](const std::string& group_label) {
    return AddCard(QString::fromStdString(group_label));
  });
  emit CardsChanged();
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
  // Still waiting on its installer (or one whose game wasn't found): the game is in drive C.
  browse_start_ = game.setup == "install" && !game.data_dir.empty() ? game.data_dir + "/drive_c"
                                                                    : game.install_path;
  source_ = game.source;
  windows_ = game.platform == "windows";
  if (web_ != (game.platform == "web")) {
    web_ = game.platform == "web";
    PopulateRunnerCombo(runners_);
  }
  noun_ = std::ranges::contains(game.tags, tags::kApp) ? "app" : "game";
  const bool emulated = game.runner_ref.starts_with("emulator:");
  change_install_->setToolTip(
      QString("Point this %1 at the %2 it was moved to").arg(noun_, emulated ? "file" : "folder"));
  change_data_dir_->setToolTip("Point this " + noun_ + " at another prefix");
  ShowInstallPath();
  data_dir_ = game.data_dir;
  ShowDataDir();
  // A desktop entry's files belong to another app, and a store keeps track of its own games.
  const bool repointable =
      !game.install_path.empty() && game.source != "desktop-entry" && game.files_kept_by.empty();
  change_install_->setVisible(repointable);
  open_install_->setVisible(!game.install_path.empty());
  missing_text_->setText(
      QString("This %1's files aren't in %2 any more. If you moved them, point it "
              "at where they are now.")
          .arg(noun_, QString::fromStdString(game.install_path)));
  missing_->setVisible(game.status == "missing" && repointable);

  detect_->setVisible(!game.install_path.empty());

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
  patch.runner_config_json = game.runner_config_json;
  patch.launch = game.launch;
  default_runner_ = game.default_runner;
  fix_known_ = game.protonfix_known;
  fix_applies_ = game.protonfix_applies;
  fix_build_ = game.protonfix_fixed_in;
  unidentified_ = game.platform == "windows" && game.identity.umu_id.empty() &&
                  game.identity.steam_appid.empty();
  ShowFixStatus();
  runner_defaults_.clear();
  runner_defaults_["gameid"] = game.identity.umu_id;
  runner_defaults_["store"] = game.identity.store;
  identity_text_->setText(IdentityText(game));
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
  data_dir_row_->setVisible(data_dir_row_->parentWidget() != this && ShowsPrefixRow(game));
  change_data_dir_->setVisible(own_files && game.source != "steam");  // Steam makes its own
  // An emulator's game names its emulator, and picking another moves it there.
  if (emulated) {
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
  show(exe_combo_->lineEdit(), patch.exe_path);
  for (QLineEdit* edit : {working_dir_edit_, exe_combo_->lineEdit()}) ShowPathStart(edit);
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

void GameEditForm::ShowFixStatus() {
  fix_row_->setVisible((fix_known_ || unidentified_) && arranged_ &&
                       fix_row_->parentWidget() != this);
  // Not knowing which game it is, Proton can't find its fix: say so where that matters.
  if (!fix_known_) {
    fix_text_->setText("Not known which game this is");
    fix_use_->setText("Set its Steam game ID");
    fix_use_->setVisible(true);
    return;
  }
  fix_text_->setText(fix_applies_ ? "Applied by its Proton" : "Not in its Proton");
  // "proton:GE-Proton9-1" reads "Use GE-Proton9-1".
  fix_use_->setText("Use " + QString::fromStdString(fix_build_).section(':', 1));
  fix_use_->setVisible(!fix_applies_ && !fix_build_.empty());
}

void GameEditForm::ShowRunnerFieldValues() {
  int set = 0;
  for (const RunnerField& field : runner_fields_) {
    const auto it = runner_config_.find(field.key);
    field.edit->setText(it == runner_config_.end() ? QString()
                        : it->is_string()          ? QString::fromStdString(it->get<std::string>())
                                                   : QString::fromStdString(it->dump()));
    field.edit->setCursorPosition(0);
    const auto shown = runner_defaults_.find(field.key);
    field.edit->setPlaceholderText(shown != runner_defaults_.end() && !shown->second.empty()
                                       ? QString::fromStdString(shown->second)
                                       : QString("Not set"));
    set += it != runner_config_.end();
  }
  runner_options_->SetSummary(set == 0 ? QString("Not set") : QString("%1 set").arg(set));
}

void GameEditForm::ShowInstallPath() {
  ShowReadOnlyPath(install_path_edit_, install_path_);
}

void GameEditForm::ShowDataDir() {
  ShowReadOnlyPath(data_dir_edit_, data_dir_);
  open_data_dir_->setEnabled(false);
  if (data_dir_.empty()) return;
  // Off the UI thread: a stale network mount can stall a stat.
  const QString path = data_dir_edit_->text();
  async::Run<bool>(
      this, [path] { return QDir(path).exists(); },
      [this, path](bool exists) {
        if (data_dir_edit_->text() == path) open_data_dir_->setEnabled(exists);
      });
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
  const QString start_dir = QString::fromStdString(browse_start_);
  const QString selected = QFileDialog::getOpenFileName(this, "Select executable", start_dir);
  if (selected.isEmpty()) return;

  exe_combo_->setEditText(RelativeIfInside(selected, install_path_));
  ShowPathStart(exe_combo_->lineEdit());
}

void GameEditForm::ChangeFolder(bool prefix) {
  // An emulator's game is the file it opens.
  const bool file = !prefix && emulator_row_->isVisibleTo(this);
  const std::string current = prefix ? data_dir_ : install_path_;
  const QString start =
      current.empty()
          ? QString()
          : QString::fromStdString(std::filesystem::path(current).parent_path().string());
  const QString picked =
      file ? QFileDialog::getOpenFileName(this, "Choose the game file",
                                          QString::fromStdString(install_path_))
           : QFileDialog::getExistingDirectory(
                 this, prefix ? "Choose the prefix folder" : "Choose the " + noun_ + "'s folder",
                 start);
  if (picked.isEmpty()) return;
  mira_gui::GamePatch patch;
  (prefix ? patch.data_dir : patch.install_path) = picked.toStdString();
  QPushButton* button = prefix ? change_data_dir_ : change_install_;
  button->setEnabled(false);
  // Saved at once, apart from the card's other edits; game.updated then loads the game again.
  api::PatchGameAsync(
      this, id_, patch, [this, button, prefix, picked](mira_gui::PatchGameResult result) {
        button->setEnabled(true);
        if (!result.ok) {
          notify::FailedRequest(this,
                                prefix ? "Could not change the prefix folder"
                                       : "Could not change the " + noun_ + "'s folder",
                                result.error);
          return;
        }
        if (prefix) {
          data_dir_ = picked.toStdString();
          ShowDataDir();
        } else {
          install_path_edit_->setText(picked);
          ShowPathStart(install_path_edit_);
        }
      });
}

void GameEditForm::DetectExecutable() {
  detect_->setEnabled(false);
  mira_gui::api::DetectGameAsync(this, id_, [this](mira_gui::PatchGameResult result) {
    detect_->setEnabled(true);
    if (!result.ok) {
      notify::FailedRequest(this, "Could not detect the executable", result.error);
      return;
    }
    Load();
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
      (now.runner_ref != original_patch_.runner_ref) + (now.launch != original_patch_.launch);
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
                              patch.runner_ref || patch.runner_config_json || patch.launch;

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
