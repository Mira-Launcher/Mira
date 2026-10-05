#include "SourcePage.h"

#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStandardItemModel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "../dialogs/AddManualGameDialog.h"
#include "../dialogs/ItchCollectionsDialog.h"
#include "../ui/ArtworkStore.h"
#include "../ui/CoverArt.h"
#include "../ui/DownloadTracker.h"
#include "../ui/ErrorHelp.h"
#include "../client/EventHub.h"
#include "../ui/GameLibraryModel.h"
#include "../ui/GameActions.h"
#include "../ui/GameTileDelegate.h"
#include "../ui/HoverCard.h"
#include "../ui/Icons.h"
#include "../ui/Notify.h"
#include "../ui/TabRow.h"
#include "../ui/Theme.h"
#include "../ui/TileGrid.h"
#include "SourceSettingsCard.h"

namespace mira_gui {
namespace {

// What differs per source, in words. Endpoints live in MiradClient.
struct SourceCopy {
  QString blurb;  // one line under the page title
  QString tool;   // stores: the helper mirad drives
  QString sign_in_steps;
  QString credential_placeholder;
  QString import_button;  // empty: no import
};

SourceCopy CopyFor(const std::string& id) {
  if (id == "steam") {
    return {"Your installed Steam games. Steam itself still installs and updates them.", "", "", "",
            "Scan Steam library"};
  }
  if (id == "epic") {
    return {"Epic Games Store, through Legendary. Games run through Mira's own Wine/Proton.",
            "Legendary",
            "Open Epic's login page and sign in, then paste the code it shows (or the whole "
            "page) here.",
            "authorizationCode, or the whole page", "Import installed games"};
  }
  if (id == "gog") {
    return {"GOG, through gogdl. Games install into your games folder.", "gogdl",
            "Open GOG's login page and sign in. It ends on a blank page: paste that page's "
            "address here.",
            "Address of the blank page, or its code", "Import installed games"};
  }
  if (id == "itch") {
    return {"itch.io, through butler.", "butler",
            "Create an API key on itch.io and paste it here. Keys don't expire.", "API key",
            "Import installed games"};
  }
  if (id == "amazon") {
    return {"Amazon Games, through nile.", "nile",
            "Open Amazon's login page and sign in. It ends on an amazon.com page: paste that "
            "page's address here.",
            "Address of the page login ends on", "Import installed games"};
  }
  if (id == "humble") {
    return {"Humble Bundle purchases, through humble-cli. Downloads land in your games folder.",
            "humble-cli",
            "Sign in to Humble Bundle in your browser, then copy the value of its "
            "_simpleauth_sess cookie (developer tools → Storage or Application → Cookies) and "
            "paste it here.",
            "_simpleauth_sess cookie value", ""};
  }
  if (id == "battlenet") {
    return {"Battle.net runs in its own Wine prefix. Games you install in it show up here.", "", "",
            "", "Import games"};
  }
  if (id == "ubisoft") {
    return {"Ubisoft Connect runs in its own Wine prefix. Games you install in it show up here.",
            "", "", "", "Import games"};
  }
  if (id == "ea") {
    return {"The EA app runs in its own Wine prefix. Games you install in it show up here.", "",
            "", "", "Import games"};
  }
  return {"Lutris's Wine and native games. Nothing is moved; they stay playable in Lutris too.",
          "", "", "", "Import Lutris games"};
}

QLabel* Text(QWidget* parent, const QString& text, const char* role = nullptr) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  if (role != nullptr) label->setProperty("role", role);
  return label;
}

void ShowLine(QLabel* label, const QString& text, const char* role) {
  label->setProperty("role", role);
  label->style()->unpolish(label);
  label->style()->polish(label);
  label->setText(text);
  label->setVisible(!text.isEmpty());
}

// mirad's messages start lowercase; this one follows a sentence. Adds mirad's hint.
void ShowError(QLabel* label, const QString& what, const ApiError& error) {
  QString text = error_help::Describe(error);
  if (!text.isEmpty()) text[0] = text[0].toUpper();
  ShowLine(label, (what + " " + text).trimmed(), "error");
}

QString Added(int added, int updated) {
  if (added == 0 && updated == 0) return "No new games found.";
  if (added == 0) return QString("No new games; %1 updated.").arg(updated);
  return QString("Added %1 game%2.").arg(added).arg(added == 1 ? "" : "s");
}

QString Heading(const QString& text, int count) {
  return count > 0 ? QString("%1  <span style='font-weight:400; opacity:0.6'>%2</span>").arg(text).arg(count)
                   : text;
}

}  // namespace

void RemoveSource(QWidget* parent, const SourceInfo& source, std::function<void()> on_removed) {
  const QString name = source.name;
  const std::string id = source.id.toStdString();
  MiradClient::GetRemovalPlanAsync(parent, id, [parent, id, name, on_removed](RemovalPlanResult plan) {
    if (!plan.ok) {
      notify::FailedRequest(parent, "Could not plan the removal.", plan.error);
      return;
    }
    QStringList lines;
    const auto uninstalled = std::ranges::count_if(plan.games, [](const auto& g) { return !g.deletes.empty(); });
    if (uninstalled > 0) lines << QString("Uninstalls %1 game%2:").arg(uninstalled).arg(uninstalled == 1 ? "" : "s");
    for (const auto& game : plan.games) {
      if (!game.deletes.empty()) lines << "  • " + QString::fromStdString(game.name);
    }
    const auto dropped = static_cast<qsizetype>(plan.games.size()) - uninstalled;
    if (dropped > 0) {
      lines << QString("Removes %1 game%2 from Mira only (their files stay where they are).")
                   .arg(dropped)
                   .arg(dropped == 1 ? "" : "s");
    }
    if (!plan.launcher_dir.empty()) lines << "Deletes " + name + " itself.";
    if (plan.signs_out) lines << "Signs you out of " + name + ".";
    if (!plan.kept.empty()) lines << "Keeps game data and saves (prefixes stay on disk).";
    lines << name + " is turned off; turn it on again in Manage sources any time.";
    if (!notify::Confirm(parent, "Remove " + name, lines.join("\n"), "Remove", /*destructive=*/true)) return;
    MiradClient::RemoveSourceAsync(parent, id, [parent, name, on_removed](RemoveSourceResult r) {
      if (!r.ok) {
        notify::FailedRequest(parent, "Could not remove " + name + ".", r.error);
        return;
      }
      if (!r.problems.empty()) {
        QStringList problems;
        for (const std::string& problem : r.problems) problems << QString::fromStdString(problem);
        notify::Failed(parent, name + " was removed, but some steps failed.", problems.join("\n"));
      }
      on_removed();
    });
  });
}

SourcePage::SourcePage(const SourceInfo& source, GameLibraryModel* library, ArtworkStore* artwork,
                       DownloadTracker* downloads, bool tabs, int tile_width, QWidget* parent)
    : QWidget(parent),
      source_(source),
      id_(source.id.toStdString()),
      library_(library),
      artwork_(artwork),
      downloads_(downloads),
      tile_(tile_width, tile_width * 3 / 2),
      use_tabs_(tabs) {
  games_ = new GameFilterProxy(library_, this);
  games_->SetSource(id_);
  owned_model_ = new QStandardItemModel(this);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* content = new QWidget();
  content_layout_ = new QVBoxLayout(content);
  content_layout_->setContentsMargins(22, 14, 22, 16);
  content_layout_->setSpacing(14);
  content_layout_->addWidget(BuildTopRow());
  content_layout_->addWidget(BuildSetupCard());
  if (id_ != "humble") content_layout_->addWidget(BuildLibrarySection());
  if (HasOwned()) content_layout_->addWidget(BuildOwnedSection());
  UpdateSections();
  content_layout_->addStretch(1);
  scroll->setWidget(content);
  // A click on the page's own background deselects, like one between tiles.
  content->installEventFilter(this);
  content_ = content;
  outer->addWidget(scroll);

  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live) HandleEvent(type, data);
          });
  connect(downloads_, &DownloadTracker::Changed, this, [this](const QString& key) {
    if (owned_grid_ != nullptr && (key.isEmpty() || key.startsWith(source_.id + ":"))) RebuildOwnedTiles();
  });
  connect(library_, &GameLibraryModel::Changed, this, &SourcePage::LibraryUpdated);
  LibraryUpdated();

  RefreshStatus();
  if (id_ == "steam") RefreshOwned();
}

bool SourcePage::eventFilter(QObject* watched, QEvent* event) {
  if (watched == content_ && event->type() == QEvent::MouseButtonPress) {
    for (TileGrid* grid : {library_grid_, owned_grid_}) {
      if (grid == nullptr) continue;
      grid->clearSelection();
      grid->setCurrentIndex(QModelIndex());
    }
  }
  return QWidget::eventFilter(watched, event);
}

bool SourcePage::HasImport() const { return !CopyFor(id_).import_button.isEmpty(); }

bool SourcePage::HasOwned() const { return IsStore() || id_ == "steam"; }

bool SourcePage::IsOwnGame(const GameSummary& game) const { return game.source == id_; }

// No title: the sidebar already says which source this is. One row holds the
// tabs, the status and every action.
QWidget* SourcePage::BuildTopRow() {
  tabs_ = new TabRow(this);
  tabs_->AddTab("installed", "Installed");
  tabs_->AddTab("owned", id_ == "humble" ? "Purchases" : "Not installed");
  tabs_->SetCurrent(id_ == "humble" ? "owned" : "installed");
  connect(tabs_, &TabRow::CurrentChanged, this, &SourcePage::UpdateSections);

  status_line_ = new QLabel(tabs_);
  status_line_->setObjectName("page_status");
  status_line_->setTextFormat(Qt::RichText);
  status_line_->setToolTip(CopyFor(id_).blurb);
  tabs_->SetTrailing(status_line_);

  // Launchers: open it. Stores: sign out.
  banner_primary_ = new QPushButton(tabs_);
  banner_primary_->setVisible(false);
  connect(banner_primary_, &QPushButton::clicked, this, [this] {
    banner_primary_->setEnabled(false);
    if (IsLauncher()) {
      MiradClient::OpenLauncherAsync(this, id_, [this](StoreActionResult result) {
        banner_primary_->setEnabled(true);
        if (!result.ok) {
          setup_card_->setVisible(true);
          ShowError(setup_error_, "Could not open " + source_.name + ".", result.error);
        }
      });
      return;
    }
    MiradClient::SignOutStoreAsync(this, id_, [this](StoreActionResult result) {
      banner_primary_->setEnabled(true);
      if (!result.ok) {
        setup_card_->setVisible(true);
        ShowError(setup_error_, "Could not sign out.", result.error);
        return;
      }
      RefreshStatus();
    });
  });
  tabs_->SetTrailing(banner_primary_);

  import_button_ = new QPushButton(CopyFor(id_).import_button, tabs_);
  import_button_->setIcon(icons::For(icons::Glyph::Refresh));
  // Stores and launchers show it once set up (ApplyStoreStatus/ApplyLauncher).
  import_button_->setVisible(HasImport() && !IsStore() && !IsLauncher());
  connect(import_button_, &QPushButton::clicked, this, &SourcePage::Import);
  tabs_->SetTrailing(import_button_);

  settings_button_ = new QToolButton(tabs_);
  settings_button_->setIcon(icons::For(icons::Glyph::Settings));
  settings_button_->setToolTip(source_.name + " settings");
  settings_button_->setCheckable(true);
  settings_button_->setAutoRaise(true);
  connect(settings_button_, &QToolButton::toggled, this, &SourcePage::ToggleSettings);
  tabs_->SetTrailing(settings_button_);

  more_button_ = new QToolButton(tabs_);
  more_button_->setText("⋯");
  more_button_->setToolTip("More");
  more_button_->setAutoRaise(true);
  more_button_->setPopupMode(QToolButton::InstantPopup);
  auto* more_menu = new QMenu(more_button_);
  connect(more_menu, &QMenu::aboutToShow, this, [this, more_menu] { FillMoreMenu(more_menu); });
  more_button_->setMenu(more_menu);
  tabs_->SetTrailing(more_button_);

  filter_ = new QLineEdit(tabs_);
  filter_->setPlaceholderText("Filter…");
  filter_->setClearButtonEnabled(true);
  connect(filter_, &QLineEdit::textChanged, this, &SourcePage::ApplyFilter);
  tabs_->SetSearch(filter_);
  return tabs_;
}

void SourcePage::ToggleSettings(bool shown) {
  if (shown && settings_card_ == nullptr) {
    settings_card_ = new SourceSettingsCard(source_, this);
    connect(settings_card_, &SourceSettingsCard::OpenSettingsRequested, this, &SourcePage::OpenSettingsRequested);
    content_layout_->insertWidget(1, settings_card_);  // right under the top row
  } else if (shown) {
    settings_card_->Refresh();
  }
  if (settings_card_ != nullptr) settings_card_->setVisible(shown);
}

void SourcePage::FillMoreMenu(QMenu* menu) {
  menu->clear();
  const QString tool = CopyFor(id_).tool;
  if (IsStore() && tool_installed_) {
    const QString version = tool_version_.empty() ? QString() : " (" + QString::fromStdString(tool_version_) + ")";
    QAction* update = menu->addAction(icons::For(icons::Glyph::Download), "Update " + tool + version, this,
                                      &SourcePage::UpdateTool);
    update->setEnabled(!tool_updating_);
    update->setToolTip("Download the latest release of " + tool + " again.");
  }
  if (IsLauncher() && launcher_installed_ && !launcher_game_id_.empty()) {
    const std::string game_id = launcher_game_id_;
    menu->addAction(icons::For(icons::Glyph::Home), "Open prefix folder", this, [this] {
      actions::OpenInstallFolder(this, launcher_prefix_);
    });
    menu->addAction(icons::For(icons::Glyph::Wrench), "Winetricks…", this,
                    [this, game_id] { actions::RunWinetricks(this, game_id, source_.name); });
    menu->addAction("Run a program in its prefix…", this, [this, game_id] {
      actions::RunInPrefix(this, game_id, launcher_prefix_ + "/drive_c", source_.name);
    });
    menu->addAction("View log", this, [this, game_id] { actions::ViewLog(this, game_id, source_.name); });
  }

  // What removing does differs per kind; RemoveSource spells it out before anything happens.
  const bool removable = library_count_ > 0 || (IsStore() ? tool_installed_ || authenticated_
                                                : IsLauncher() ? launcher_installed_
                                                               : true);
  if (removable) {
    if (!menu->isEmpty()) menu->addSeparator();
    const QString label = IsLauncher() ? "Uninstall " + source_.name + "…"
                          : IsStore()  ? "Remove " + source_.name + "…"
                                       : "Remove from Mira…";
    menu->addAction(icons::For(icons::Glyph::Trash, theme::Current().error), label, this, [this] {
      RemoveSource(this, source_, [this] { emit Removed(); });
    });
  }
  if (menu->isEmpty()) menu->addAction("Nothing to manage until it's set up")->setEnabled(false);
}

void SourcePage::UpdateTool() {
  tool_updating_ = true;
  UpdateStatusLine();
  MiradClient::SetupStoreToolAsync(this, id_, [this](StoreActionResult result) {
    tool_updating_ = false;
    if (result.ok) {
      RefreshStatus();
      return;
    }
    UpdateStatusLine();
    setup_card_->setVisible(true);
    ShowError(setup_error_, "Could not update " + CopyFor(id_).tool + ".", result.error);
  });
}

QWidget* SourcePage::BuildSetupCard() {
  const SourceCopy copy = CopyFor(id_);
  auto* card = new mira_gui::SettingsCard("Set up " + source_.name, this);
  setup_card_ = card;
  setup_card_->setVisible(false);

  QStringList titles;
  if (IsStore()) titles << "Get " + copy.tool << "Sign in to " + source_.name;
  if (IsLauncher()) titles << "Install " + source_.name;
  if (!titles.isEmpty() && HasImport()) titles << "Import your games";
  for (int i = 0; i < titles.size(); ++i) {
    Step step;
    step.row = new QWidget(card);
    auto* column = new QVBoxLayout(step.row);
    column->setContentsMargins(18, 10, 18, 10);
    column->setSpacing(8);
    auto* line = new QHBoxLayout();
    line->setSpacing(12);
    step.marker = new QLabel(QString::number(i + 1), step.row);
    step.marker->setObjectName("step_marker");
    step.marker->setFixedSize(26, 26);
    step.marker->setAlignment(Qt::AlignCenter);
    line->addWidget(step.marker);
    step.title = new QLabel(titles[i], step.row);
    step.title->setObjectName("step_title");
    line->addWidget(step.title, /*stretch=*/1);
    column->addLayout(line);
    card->AddRow(step.row);
    steps_.push_back(step);
  }

  // Moved into whichever step is current (SetStep).
  setup_body_ = new QWidget(card);
  auto* body = new QVBoxLayout(setup_body_);
  body->setContentsMargins(38, 0, 0, 4);
  body->setSpacing(8);
  setup_text_ = Text(setup_body_, QString());
  body->addWidget(setup_text_);

  setup_button_ = new QPushButton(setup_body_);
  setup_button_->setIcon(icons::For(icons::Glyph::Download, theme::Current().on_accent));
  setup_button_->setDefault(true);
  setup_button_->setVisible(false);
  connect(setup_button_, &QPushButton::clicked, this, [this] {
    setup_button_->setEnabled(false);
    setup_error_->setVisible(false);
    const auto failed = [this](StoreActionResult result) {
      if (result.ok) return;  // the event finishes the job
      launcher_installing_ = false;
      setup_button_->setEnabled(true);
      ShowError(setup_error_, "Could not start it.", result.error);
    };
    if (IsLauncher()) {
      launcher_installing_ = true;
      setup_button_->setText("Installing…");
      UpdateStatusLine();
      MiradClient::InstallLauncherAsync(this, id_, failed);
    } else {
      setup_button_->setText("Downloading…");
      MiradClient::SetupStoreToolAsync(this, id_, [this](StoreActionResult result) {
        if (result.ok) {
          RefreshStatus();
          return;
        }
        setup_card_->setVisible(true);
        setup_button_->setEnabled(true);
        setup_button_->setText("Retry download");
        ShowError(setup_error_, "It failed.", result.error);
        UpdateStatusLine();
      });
    }
  });
  auto* button_row = new QHBoxLayout();
  button_row->addWidget(setup_button_);
  button_row->addStretch(1);
  body->addLayout(button_row);

  sign_in_row_ = new QWidget(setup_body_);
  sign_in_row_->setVisible(false);
  auto* sign_in_layout = new QHBoxLayout(sign_in_row_);
  sign_in_layout->setContentsMargins(0, 0, 0, 0);
  open_login_ = new QPushButton("Open login page", sign_in_row_);
  connect(open_login_, &QPushButton::clicked, this, &SourcePage::OpenLogin);
  credential_ = new QLineEdit(sign_in_row_);
  credential_->setPlaceholderText(copy.credential_placeholder);
  connect(credential_, &QLineEdit::returnPressed, this, &SourcePage::SignIn);
  sign_in_ = new QPushButton("Sign in", sign_in_row_);
  sign_in_->setDefault(true);
  connect(sign_in_, &QPushButton::clicked, this, &SourcePage::SignIn);
  sign_in_layout->addWidget(open_login_);
  sign_in_layout->addWidget(credential_, /*stretch=*/1);
  sign_in_layout->addWidget(sign_in_);
  body->addWidget(sign_in_row_);

  setup_error_ = Text(setup_body_, QString(), "error");
  setup_error_->setVisible(false);
  body->addWidget(setup_error_);
  setup_body_->hide();
  return setup_card_;
}

void SourcePage::SetStep(int current) {
  for (int i = 0; i < static_cast<int>(steps_.size()); ++i) {
    const Step& step = steps_[i];
    const char* state = i < current ? "done" : i == current ? "current" : "todo";
    step.marker->setText(i < current ? QString::fromUtf8("\xe2\x9c\x93") : QString::number(i + 1));
    step.marker->setProperty("state", state);
    step.marker->style()->unpolish(step.marker);
    step.marker->style()->polish(step.marker);
    step.title->setProperty("state", state);
    step.title->style()->unpolish(step.title);
    step.title->style()->polish(step.title);
    // In code: a stylesheet font-weight on a property state didn't apply here.
    QFont font = step.title->font();
    font.setWeight(i == current ? QFont::DemiBold : QFont::Normal);
    step.title->setFont(font);
  }
  if (current >= 0 && current < static_cast<int>(steps_.size())) {
    static_cast<QVBoxLayout*>(steps_[current].row->layout())->addWidget(setup_body_);
    setup_body_->show();
  }
}

QWidget* SourcePage::BuildLibrarySection() {
  auto* section = new QWidget(this);
  library_section_ = section;
  auto* layout = new QVBoxLayout(section);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* header = new QHBoxLayout();
  library_heading_ = new QLabel("In your library", section);
  library_heading_->setProperty("role", "heading");
  header->addWidget(library_heading_);
  header->addStretch(1);
  import_result_ = Text(section, QString(), "muted");
  import_result_->setWordWrap(false);
  import_result_->setVisible(false);
  header->addWidget(import_result_);
  layout->addLayout(header);

  library_empty_ = Text(section, QString(), "muted");
  layout->addWidget(library_empty_);

  library_grid_ = new TileGrid(tile_, artwork_, section);
  library_grid_->setModel(games_);
  library_grid_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(library_grid_, &QWidget::customContextMenuRequested, this, &SourcePage::ShowLibraryMenu);
  connect(library_grid_, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
    emit PlayRequested(index.data(GameTileDelegate::IdRole).toString());
  });
  library_grid_->on_hover = [this](const QModelIndex& index) { ShowHoverCard(library_grid_, index); };
  library_grid_->on_ctrl_wheel = [this](int steps) { emit ZoomRequested(steps); };
  layout->addWidget(library_grid_);
  return section;
}

QWidget* SourcePage::BuildOwnedSection() {
  owned_section_ = new QWidget(this);
  owned_available_ = id_ == "steam";
  auto* layout = new QVBoxLayout(owned_section_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* header = new QHBoxLayout();
  owned_heading_ = new QLabel(id_ == "humble" ? "Your purchases" : "Not installed", owned_section_);
  owned_heading_->setProperty("role", "heading");
  header->addWidget(owned_heading_);
  header->addStretch(1);
  owned_refresh_ = new QPushButton("Refresh", owned_section_);
  owned_refresh_->setIcon(icons::For(icons::Glyph::Refresh));
  connect(owned_refresh_, &QPushButton::clicked, this, &SourcePage::RefreshOwned);
  if (id_ == "itch") {
    auto* collections = new QPushButton("Manage collections…", owned_section_);
    collections->setToolTip("Show games from itch.io collections here. Add your own or any collection by link.");
    connect(collections, &QPushButton::clicked, this, [this] {
      ItchCollectionsDialog dialog(this);
      dialog.exec();
      if (dialog.Changed()) RefreshOwned();
    });
    header->addWidget(collections);
  }
  header->addWidget(owned_refresh_);
  layout->addLayout(header);

  owned_note_ = Text(owned_section_, QString(), "muted");
  owned_note_->setVisible(false);
  layout->addWidget(owned_note_);
  if (id_ == "steam") {
    steam_settings_ = new QPushButton("Open Steam settings", owned_section_);
    steam_settings_->setVisible(false);
    connect(steam_settings_, &QPushButton::clicked, this,
            [this] { emit OpenSettingsRequested("steam.web_api_key"); });
    auto* row = new QHBoxLayout();
    row->addWidget(steam_settings_);
    row->addStretch(1);
    layout->addLayout(row);
  }
  if (id_ != "humble") {
    art_key_ = new QPushButton("Add a SteamGridDB key for covers", owned_section_);
    art_key_->setIcon(icons::For(icons::Glyph::Image));
    art_key_->setVisible(false);
    connect(art_key_, &QPushButton::clicked, this, [this] { emit OpenSettingsRequested(art_key_setting_); });
    auto* row = new QHBoxLayout();
    row->addWidget(art_key_);
    row->addStretch(1);
    layout->addLayout(row);
  }

  owned_grid_ = new TileGrid(tile_, artwork_, owned_section_);
  owned_grid_->setModel(owned_model_);
  owned_grid_->on_hover = [this](const QModelIndex& index) { ShowHoverCard(owned_grid_, index); };
  owned_grid_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(owned_grid_, &QWidget::customContextMenuRequested, this, &SourcePage::ShowOwnedMenu);
  owned_grid_->on_ctrl_wheel = [this](int steps) { emit ZoomRequested(steps); };
  owned_grid_->on_action = [this](const QModelIndex& index) {
    const QString ref = index.data(GameTileDelegate::IdRole).toString();
    if (id_ != "humble") {
      StartInstall(ref, /*update=*/false);
      return;
    }
    if (humble_paths_.contains(ref)) {
      AddManualGameDialog dialog(this);
      dialog.Prefill(humble_paths_.value(ref), index.data(GameTileDelegate::NameRole).toString());
      dialog.exec();
      return;
    }
    owned_state_.insert(ref, "Downloading…");
    RebuildOwnedTiles();
    MiradClient::DownloadHumbleBundleAsync(this, ref.toStdString(), [this, ref](HumbleDownloadResult r) {
      if (r.ok) {
        owned_state_.remove(ref);
        humble_paths_.insert(ref, QString::fromStdString(r.path));
      } else if (r.error.code == "nothing_to_download") {
        owned_state_.insert(ref, "Nothing to download");  // e.g. only a Steam key
      } else {
        owned_state_.remove(ref);
        ShowError(owned_note_, "The download failed.", r.error);
      }
      RebuildOwnedTiles();
    });
  };
  // Double-click does what the tile's button does, while it's clickable.
  connect(owned_grid_, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
    if (index.data(GameTileDelegate::ActionEnabledRole).toBool() && owned_grid_->on_action) owned_grid_->on_action(index);
  });
  layout->addWidget(owned_grid_);
  return owned_section_;
}

void SourcePage::LibraryUpdated() {
  library_count_ = static_cast<int>(
      std::ranges::count_if(library_->Games(), [this](const GameSummary& game) { return IsOwnGame(game); }));
  if (library_heading_ != nullptr) library_heading_->setText(Heading("In your library", library_count_));
  tabs_->SetCount("installed", library_count_);
  UpdateStatusLine();
  if (library_grid_ != nullptr) library_grid_->FitHeight();
}

void SourcePage::SetTileWidth(int width) {
  const QSize tile(width, width * 3 / 2);
  if (tile == tile_) return;
  tile_ = tile;
  // The library grid's delegate draws its covers at its own size.
  if (library_grid_ != nullptr) library_grid_->SetTileSize(tile_);
  if (owned_grid_ != nullptr) {
    owned_grid_->SetTileSize(tile_);
    RebuildOwnedTiles();
  }
}

void SourcePage::UpdateCover(const QString& id) {
  // A not-installed title's cover is keyed "<source>-<ref>". The library
  // grid's own tiles repaint from the shared model.
  const QString prefix = source_.id + "-";
  if (owned_grid_ == nullptr || id_ == "humble" || !id.startsWith(prefix)) return;
  const QString ref = id.mid(prefix.size());
  for (int row = 0; row < owned_model_->rowCount(); ++row) {
    QStandardItem* item = owned_model_->item(row);
    if (item->data(GameTileDelegate::IdRole).toString() != ref) continue;
    item->setData(artwork_->TitleCover(source_.id, ref, item->data(GameTileDelegate::NameRole).toString(), tile_,
                                       devicePixelRatioF()),
                  Qt::DecorationRole);
  }
}

void SourcePage::RefreshStatus() {
  if (IsStore()) {
    MiradClient::GetStoreStatusAsync(this, id_, [this](StoreStatusResult s) { ApplyStoreStatus(s); });
  } else if (IsLauncher()) {
    MiradClient::GetLaunchersAsync(this, [this](LaunchersResult result) {
      if (!result.ok) {
        setup_card_->setVisible(true);
        ShowError(setup_error_, "Could not ask mirad about " + source_.name + ".", result.error);
        return;
      }
      for (const LauncherInfo& launcher : result.launchers) {
        if (launcher.id == id_) ApplyLauncher(launcher);
      }
    });
  } else {
    UpdateStatusLine();
  }
}

void SourcePage::ApplyStoreStatus(const StoreStatusResult& status) {
  const SourceCopy copy = CopyFor(id_);
  if (!status.ok) {
    setup_card_->setVisible(true);
    ShowError(setup_error_, "Could not ask mirad about " + source_.name + ".", status.error);
    return;
  }
  const bool was_authenticated = authenticated_;
  tool_installed_ = status.tool_installed;
  tool_version_ = status.tool_version;
  authenticated_ = status.authenticated;
  account_ = status.account;
  setup_error_->setVisible(false);

  // One step at a time: the tool, then the account.
  setup_card_->setVisible(!tool_installed_ || !authenticated_);
  setup_button_->setVisible(!tool_installed_);
  setup_button_->setEnabled(true);
  sign_in_row_->setVisible(tool_installed_ && !authenticated_);
  SetStep(!tool_installed_ ? 0 : !authenticated_ ? 1 : 2);
  if (!tool_installed_) {
    setup_text_->setText("Mira uses " + copy.tool + " to talk to " + source_.name +
                         ". It's downloaded once, from its own releases.");
    setup_button_->setText("Download " + copy.tool);
  } else if (!authenticated_) {
    setup_text_->setText(copy.sign_in_steps);
    open_login_->setVisible(true);
  }

  banner_primary_->setText("Sign out");
  banner_primary_->setVisible(authenticated_ && id_ != "humble");
  if (import_button_ != nullptr) import_button_->setVisible(tool_installed_ && HasImport());
  owned_available_ = tool_installed_ && authenticated_;
  if (owned_section_ != nullptr && authenticated_ && !was_authenticated) RefreshOwned();
  UpdateSections();
  UpdateStatusLine();
}

void SourcePage::ApplyLauncher(const LauncherInfo& launcher) {
  const bool was_installed = launcher_installed_;
  launcher_installed_ = launcher.installed;
  launcher_installing_ = launcher.install_state == "running";
  launcher_game_id_ = launcher.game_id;
  launcher_prefix_ = launcher.prefix;
  // Its runner row only works once there's a prefix.
  if (launcher_installed_ != was_installed && settings_card_ != nullptr) settings_card_->Refresh();
  setup_card_->setVisible(!launcher_installed_);
  SetStep(launcher_installed_ ? 1 : 0);
  if (!launcher_installed_) {
    setup_text_->setText(
        launcher.interactive_install
            ? "Mira makes a Wine prefix for it and runs its installer. The installer's window "
              "opens: click through it, then sign in."
            : "Mira makes a Wine prefix for it and installs it there silently. Sign in once it "
              "opens.");
    setup_button_->setVisible(true);
    setup_button_->setEnabled(!launcher_installing_);
    setup_button_->setText(launcher_installing_ ? "Installing…" : "Install " + source_.name);
    if (launcher.install_state == "failed" && !launcher.error.empty()) {
      ShowError(setup_error_, "The last install failed.", launcher.error);
    }
  }
  banner_primary_->setText("Open " + source_.name);
  banner_primary_->setVisible(launcher_installed_);
  import_button_->setVisible(launcher_installed_);
  UpdateStatusLine();
}

void SourcePage::UpdateStatusLine() {
  const theme::Tokens& tokens = theme::Current();
  QStringList parts;
  if (IsStore()) {
    if (tool_updating_) {
      parts << StatusDot(tokens.info) + "Updating " + CopyFor(id_).tool + "…";
    } else if (!tool_installed_) {
      parts << StatusDot(tokens.warning) + "Not set up";
    } else if (!authenticated_) {
      parts << StatusDot(tokens.warning) + "Not signed in";
    } else {
      parts << StatusDot(tokens.success) +
                   (account_.empty() ? QString("Signed in") : "Signed in as " + QString::fromStdString(account_).toHtmlEscaped());
    }
  } else if (IsLauncher()) {
    parts << (launcher_installing_ ? StatusDot(tokens.info) + "Installing…"
              : launcher_installed_ ? StatusDot(tokens.success) + "Installed"
                                    : StatusDot(tokens.warning) + "Not installed");
  }
  if (id_ != "humble") {
    parts << (library_count_ == 1 ? QString("1 game in your library")
                                  : QString("%1 games in your library").arg(library_count_));
  }
  status_line_->setText(parts.join("  ·  "));

  if (library_empty_ == nullptr) return;
  library_empty_->setVisible(library_count_ == 0);
  library_grid_->setVisible(library_count_ > 0);
  if (IsStore() && !authenticated_) {
    library_empty_->setText("Games from " + source_.name + " show up here once you're signed in.");
  } else if (IsLauncher() && !launcher_installed_) {
    library_empty_->setText("Games you install through " + source_.name + " show up here.");
  } else if (HasImport()) {
    library_empty_->setText("Nothing from " + source_.name + " in your library yet. \"" +
                            CopyFor(id_).import_button + "\" brings in what's already installed.");
  }
}

void SourcePage::UpdateSections() {
  const bool has_owned = owned_section_ != nullptr && owned_available_;
  // Tabs only when there's a choice to make.
  const bool tabbed = use_tabs_ && has_owned && library_section_ != nullptr;
  tabs_->SetTabsVisible(tabbed);
  const QString current = tabs_->Current();
  if (library_section_ != nullptr) library_section_->setVisible(!tabbed || current == "installed");
  if (owned_section_ != nullptr) owned_section_->setVisible(has_owned && (!tabbed || current == "owned"));
  // A tab names its section already.
  if (library_heading_ != nullptr) library_heading_->setVisible(!tabbed);
  if (owned_heading_ != nullptr) owned_heading_->setVisible(!tabbed);
}

void SourcePage::OpenLogin() {
  open_login_->setEnabled(false);
  MiradClient::BeginStoreLoginAsync(this, id_, [this](LoginUrlResult result) {
    open_login_->setEnabled(true);
    if (!result.ok) {
      ShowError(setup_error_, "Could not start the login.", result.error);
      return;
    }
    QDesktopServices::openUrl(QUrl(QString::fromStdString(result.url)));
  });
}

void SourcePage::SignIn() {
  const QString pasted = credential_->text().trimmed();
  if (pasted.isEmpty()) return;
  sign_in_->setEnabled(false);
  sign_in_->setText("Signing in…");
  setup_error_->setVisible(false);
  MiradClient::SignInStoreAsync(this, id_, pasted.toStdString(), [this](StoreActionResult result) {
    sign_in_->setEnabled(true);
    sign_in_->setText("Sign in");
    if (!result.ok) {
      ShowError(setup_error_, "Could not sign in.", result.error);
      return;
    }
    credential_->clear();
    RefreshStatus();
  });
}

void SourcePage::Import() {
  import_button_->setEnabled(false);
  import_result_->setVisible(false);
  const auto done = [this](bool ok, const std::string& error, int added, int updated) {
    import_button_->setEnabled(true);
    if (ok) {
      ShowLine(import_result_, Added(added, updated), "muted");
    } else {
      ShowError(import_result_, "Could not import.", error);
    }
    if (ok && (added > 0 || updated > 0)) emit LibraryChanged();
  };
  if (id_ == "steam") {
    MiradClient::ScanSteamAsync(this, [done](SteamScanResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (id_ == "lutris") {
    MiradClient::ImportLutrisAsync(this, [done](LutrisImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (IsLauncher()) {
    MiradClient::ImportLauncherAsync(this, id_,
                                     [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else {
    MiradClient::ImportStoreAsync(this, id_,
                                  [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  }
}

void SourcePage::RefreshOwned() {
  owned_refresh_->setEnabled(false);
  ShowLine(owned_note_, "Loading…", "muted");
  if (id_ == "humble") {
    MiradClient::GetHumbleLibraryAsync(this, [this](HumbleLibraryResult r) { ShowBundles(r); });
  } else {
    MiradClient::GetStoreLibraryAsync(this, id_, [this](StoreLibraryResult r) { ShowOwned(r); });
  }
}

void SourcePage::ShowOwned(const StoreLibraryResult& result) {
  owned_refresh_->setEnabled(true);
  owned_.clear();
  not_owned_.clear();
  if (steam_settings_ != nullptr) steam_settings_->setVisible(false);
  if (!result.ok) {
    ShowError(owned_note_, "Could not list your games.", result.error);
    RebuildOwnedTiles();
    return;
  }
  std::vector<StoreTitle> uninstalled;
  for (const StoreTitle& title : result.titles) {
    downloads_->NoteTitle(source_.id, QString::fromStdString(title.ref), QString::fromStdString(title.title));
    if (!title.installed) {
      owned_.emplace_back(QString::fromStdString(title.ref), QString::fromStdString(title.title));
      if (!title.owned) not_owned_.insert(QString::fromStdString(title.ref));
      uninstalled.push_back(title);
    }
  }
  // Covers already fetched are skipped; the rest arrive as events.
  if (!uninstalled.empty()) {
    MiradClient::QueueTitleArtworkAsync(this, id_, std::move(uninstalled), [](StoreActionResult) {});
  }
  if (result.titles.empty() && id_ == "steam") {
    ShowLine(owned_note_,
             "Set a Steam Web API key and your SteamID64 in Settings to see the games you own "
             "but haven't installed.",
             "muted");
    if (steam_settings_ != nullptr) steam_settings_->setVisible(true);
  } else if (owned_.empty()) {
    ShowLine(owned_note_, "Everything you own is installed.", "muted");
  } else {
    ShowLine(owned_note_,
             id_ == "steam" ? "Install hands the game to the Steam client; scan the Steam library "
                              "once it's done."
                            : QString(),
             "muted");
  }
  RebuildOwnedTiles();
}

void SourcePage::ShowBundles(const HumbleLibraryResult& result) {
  owned_refresh_->setEnabled(true);
  owned_.clear();
  if (!result.ok) {
    ShowError(owned_note_, "Could not list your purchases.", result.error);
    RebuildOwnedTiles();
    return;
  }
  for (const HumbleBundle& bundle : result.bundles) {
    owned_.emplace_back(QString::fromStdString(bundle.key), QString::fromStdString(bundle.name));
    downloads_->NoteTitle("humble", QString::fromStdString(bundle.key), QString::fromStdString(bundle.name));
  }
  ShowLine(owned_note_,
           owned_.empty() ? QString("No purchases on this account.")
                          : QString("Downloads land in your games folder, where Mira picks up "
                                    "anything it recognizes."),
           "muted");
  RebuildOwnedTiles();
}

void SourcePage::RebuildOwnedTiles() {
  // The same titles in the same order (every progress tick): roles updated
  // in place, so the hover card and selection stay put.
  bool same = owned_model_->rowCount() == static_cast<int>(owned_.size());
  for (int row = 0; same && row < owned_model_->rowCount(); ++row) {
    same = owned_model_->item(row)->data(GameTileDelegate::IdRole).toString() == owned_[row].first;
  }
  if (!same) {
    ShowHoverCard(nullptr, QModelIndex());
    owned_model_->clear();
    for (const auto& [ref, title] : owned_) {
      auto* item = new QStandardItem();
      item->setData(ref, GameTileDelegate::IdRole);
      item->setData(title, GameTileDelegate::NameRole);
      item->setData(QString("ready"), GameTileDelegate::StatusRole);
      owned_model_->appendRow(item);
    }
  }
  const QString idle = id_ == "humble" ? "Download" : "Install";
  for (int row = 0; row < owned_model_->rowCount(); ++row) {
    QStandardItem* item = owned_model_->item(row);
    const auto& [ref, title] = owned_[row];
    // Bundles aren't games, so there's no cover to look up.
    item->setData(id_ == "humble" ? PlaceholderCover(title, source_.id + "-" + ref, tile_, devicePixelRatioF())
                                  : artwork_->TitleCover(source_.id, ref, title, tile_, devicePixelRatioF()),
                  Qt::DecorationRole);
    QString state = owned_state_.value(ref);
    QVariant progress;  // cleared once the install stops: the item is reused
    const DownloadTracker::Entry* running = downloads_->Find(source_.id + ":" + ref);
    if (running != nullptr && running->state == DownloadTracker::State::Running) {
      state = id_ == "humble" ? "Downloading…" : running->update ? "Updating…" : "Installing…";
      if (running->progress >= 0) {
        state = DownloadTracker::ProgressText(*running, /*short_form=*/true);
        progress = running->progress;
      }
    }
    item->setData(progress, GameTileDelegate::ProgressRole);
    if (not_owned_.contains(ref)) {
      item->setData(QString("Not owned"), GameTileDelegate::ActionRole);
      item->setData(false, GameTileDelegate::ActionEnabledRole);
      item->setToolTip(title + "\nA paid game from a collection. Buy it on itch.io to install it here.");
      continue;
    }
    const QString action = humble_paths_.contains(ref) ? QString("Add to library…") : idle;
    item->setData(state.isEmpty() ? action : state, GameTileDelegate::ActionRole);
    item->setData(state.isEmpty(), GameTileDelegate::ActionEnabledRole);
  }
  owned_heading_->setText(Heading(id_ == "humble" ? "Your purchases" : "Not installed",
                                  static_cast<int>(owned_.size())));
  tabs_->SetCount("owned", static_cast<int>(owned_.size()));
  ApplyFilter();
}

void SourcePage::StartInstall(const QString& ref, bool update) {
  owned_state_.insert(ref, update ? "Updating…" : "Installing…");
  if (owned_grid_ != nullptr) RebuildOwnedTiles();
  MiradClient::InstallStoreTitleAsync(this, id_, ref.toStdString(), update,
                                      [this, ref](StoreActionResult r) {
                                        if (r.ok) return;  // events report the rest
                                        owned_state_.remove(ref);
                                        if (owned_grid_ != nullptr) RebuildOwnedTiles();
                                        // import_result_ is hidden with its section on the owned tab.
                                        ShowError(owned_note_ != nullptr && !library_section_->isVisibleTo(this) ? owned_note_
                                                                                                                  : import_result_,
                                                  "Could not start it.", r.error);
                                      });
}

void SourcePage::ApplyFilter() {
  const QString needle = filter_->text().trimmed();
  if (library_grid_ != nullptr) {
    games_->SetSearch(needle);
    library_grid_->FitHeight();
  }
  if (owned_grid_ != nullptr) {
    for (int row = 0; row < owned_model_->rowCount(); ++row) {
      const QString name = owned_model_->item(row)->data(GameTileDelegate::NameRole).toString();
      owned_grid_->setRowHidden(row, !needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive));
    }
    owned_grid_->FitHeight();
  }
}

void SourcePage::ShowHoverCard(TileGrid* grid, const QModelIndex& index) {
  if (!index.isValid()) {
    if (hover_card_ != nullptr) hover_card_->hide();
    return;
  }
  if (hover_card_ == nullptr) hover_card_ = new HoverCard(this);
  if (grid == library_grid_) {
    const GameSummary* game = games_->GameAt(index);
    if (game == nullptr) return;
    hover_card_->ShowGame(*game, game->running);
  } else {
    // A tile's pill says what's under way; an idle one just says Install.
    const QString ref = index.data(GameTileDelegate::IdRole).toString();
    QString status = index.data(GameTileDelegate::ActionRole).toString();
    if (humble_paths_.contains(ref)) {
      status = "Downloaded to " + humble_paths_.value(ref);
    } else if (index.data(GameTileDelegate::ActionEnabledRole).toBool()) {
      status = id_ == "humble" ? "Not downloaded" : "Not installed";
    }
    hover_card_->ShowTitle(index.data(GameTileDelegate::NameRole).toString(), status, source_.name);
  }
  const QRect tile = grid->visualRect(index);
  hover_card_->PopUpBeside(QRect(grid->viewport()->mapToGlobal(tile.topLeft()), tile.size()));
}

void SourcePage::SetDragSelectEnabled(bool enabled) {
  for (TileGrid* grid : {library_grid_, owned_grid_}) {
    if (grid != nullptr) grid->SetDragSelectEnabled(enabled);
  }
}

// Right-clicking outside the selection replaces it, as in the library grid.
QModelIndexList SelectForMenu(TileGrid* grid, const QModelIndex& index) {
  if (!grid->selectionModel()->isSelected(index)) {
    grid->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect);
  }
  return grid->selectionModel()->selectedIndexes();
}

void SourcePage::ShowLibraryMenu(const QPoint& pos) {
  const QModelIndex index = library_grid_->indexAt(pos);
  if (!index.isValid()) return;
  if (const QModelIndexList selected = SelectForMenu(library_grid_, index); selected.size() > 1) {
    QStringList ids;
    for (const QModelIndex& it : selected) ids << it.data(GameTileDelegate::IdRole).toString();
    emit BatchMenuRequested(ids, library_grid_->viewport()->mapToGlobal(pos));
    return;
  }
  const QString id = index.data(GameTileDelegate::IdRole).toString();
  // A store game's id is "<source>-<ref>"; those can also be updated.
  const QString prefix = source_.id + "-";
  const QString update_ref = IsStore() && id_ != "humble" && id.startsWith(prefix) ? id.mid(prefix.size()) : QString();
  emit GameMenuRequested(id, library_grid_->viewport()->mapToGlobal(pos), update_ref);
}

void SourcePage::ShowOwnedMenu(const QPoint& pos) {
  const QModelIndex index = owned_grid_->indexAt(pos);
  if (!index.isValid()) return;
  QStringList refs;
  for (const QModelIndex& it : SelectForMenu(owned_grid_, index)) {
    if (it.data(GameTileDelegate::ActionEnabledRole).toBool()) refs << it.data(GameTileDelegate::IdRole).toString();
  }
  // One downloaded bundle adds to the library; several only download the rest.
  const bool add = refs.size() == 1 && humble_paths_.contains(refs.front());
  if (!add) refs.removeIf([this](const QString& ref) { return humble_paths_.contains(ref); });
  QMenu menu(this);
  const QString verb = add ? "Add to library…" : id_ == "humble" ? "Download" : "Install";
  QAction* start = menu.addAction(refs.size() > 1 ? QString("%1 (%2)").arg(verb).arg(refs.size()) : verb);
  start->setEnabled(!refs.isEmpty());
  if (menu.exec(owned_grid_->viewport()->mapToGlobal(pos)) != start || !owned_grid_->on_action) return;
  // Looked up again per title: each action can rebuild the tiles.
  for (const QString& ref : refs) {
    for (int row = 0; row < owned_model_->rowCount(); ++row) {
      const QModelIndex match = owned_model_->index(row, 0);
      if (match.data(GameTileDelegate::IdRole).toString() != ref) continue;
      owned_grid_->on_action(match);
      break;
    }
  }
}

void SourcePage::UpdateTitle(const QString& ref) { StartInstall(ref, /*update=*/true); }

void SourcePage::HandleEvent(const std::string& type, const std::string& data) {
  if (StoreEvent art; MiradClient::ParseTitleArtworkEvent(type, data, &art)) {
    // "ready" is LibraryWindow's: it has to land while this page is closed too.
    if (art.source == id_ && art.state == "failed" && art.error.code == "no_steamgriddb_key" &&
        art.error.fix.kind == "setting" && art_key_ != nullptr) {
      art_key_setting_ = QString::fromStdString(art.error.fix.target);
      art_key_->setToolTip(error_help::HintFor(art.error));
      art_key_->setVisible(true);
    }
    return;
  }

  StoreEvent event;
  if (!MiradClient::ParseStoreEvent(type, data, &event) || event.source != id_) return;

  if (event.kind == "setup") {
    if (event.state == "finished") {
      launcher_installing_ = false;
      tool_updating_ = false;
      RefreshStatus();
    } else if (event.state == "failed") {
      launcher_installing_ = false;
      tool_updating_ = false;
      setup_card_->setVisible(true);
      if (IsStore() && tool_installed_) {
        setup_text_->setText("Updating " + CopyFor(id_).tool + " failed.");
      }
      setup_button_->setEnabled(true);
      setup_button_->setText(IsLauncher() ? "Install " + source_.name : "Retry download");
      ShowError(setup_error_, "It failed.", event.error);
      UpdateStatusLine();
    }
    return;
  }

  const QString ref = QString::fromStdString(event.ref);
  if (event.state == "failed") {
    owned_state_.remove(ref);
    ShowError(owned_note_ != nullptr ? owned_note_ : import_result_, "It failed.", event.error);
  } else if (event.state == "finished") {
    if (event.kind == "download" && !event.downloaded) {
      owned_state_.insert(ref, "Nothing to download");  // e.g. only a Steam key
    } else if (event.kind == "download") {
      owned_state_.remove(ref);
      humble_paths_.insert(ref, QString::fromStdString(event.path));
    } else if (id_ == "steam") {
      owned_state_.insert(ref, "Sent to Steam");
    } else {
      // Now a tracked game: it moves to "In your library" on the next relist.
      owned_state_.remove(ref);
      std::erase_if(owned_, [&ref](const auto& entry) { return entry.first == ref; });
    }
  } else {
    return;
  }
  if (owned_grid_ != nullptr) RebuildOwnedTiles();
}

}  // namespace mira_gui
