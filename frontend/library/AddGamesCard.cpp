#include "AddGamesCard.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>
#include <memory>

#include "../app/ErrorHelp.h"
#include "../client/JsonMapping.h"
#include "../client/api/Config.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../game/CardParts.h"
#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"
#include "../widgets/Labels.h"
#include "../widgets/WindowCards.h"
#include "LibraryActions.h"

namespace mira_gui {
namespace {

constexpr int kWidth = 560;
constexpr int kTile = 30;
constexpr int kEntryIcon = 32;

QString Expanded(const QString& path) {
  return path.startsWith("~/") ? QDir::homePath() + path.mid(1) : path;
}

QStringList ListSetting(const ConfigResult& config, const std::string& key) {
  QStringList list;
  if (const auto it = config.values.find(key); it != config.values.end()) {
    for (const std::string& item : mapping::ParseListText(it->second))
      list << Expanded(QString::fromStdString(item));
  }
  return list;
}

// The glyph on a tile before each of the first page's rows.
QLabel* Tile(QWidget* parent, icons::Glyph glyph) {
  auto* tile = new QLabel(parent);
  tile->setProperty("role", "tile_icon");
  tile->setFixedSize(kTile, kTile);
  tile->setAlignment(Qt::AlignCenter);
  icons::Follow(tile, glyph, 18, &theme::Tokens::accent);
  return tile;
}

// One line of muted text under a row's label, cut short rather than wrapped, so the card keeps its
// height as the line fills in.
QLabel* Detail(QWidget* parent, const QString& text) {
  auto* label = new ElidedLabel(text, parent);
  label->setProperty("role", "muted");
  return label;
}

SettingsCard* NewPage() {
  auto* page = new SettingsCard(QString());
  page->setFixedWidth(kWidth);
  page->SetProminentTitle();
  return page;
}

QPixmap EntryIcon(const std::string& icon, qreal ratio) {
  const QString name = QString::fromStdString(icon);
  QIcon found = name.startsWith('/') ? QIcon(name) : QIcon::fromTheme(name);
  if (found.isNull()) found = icons::For(icons::Glyph::Grid);
  QPixmap pixmap = found.pixmap(QSize(kEntryIcon, kEntryIcon), ratio);
  return pixmap;
}

}  // namespace

AddGamesCard::AddGamesCard(QWidget* parent) : QWidget(parent) {
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  ShowMenu();
  api::GetConfigAsync(this, [this](ConfigResult config) {
    if (!config.ok) return;
    library_roots_ = ListSetting(config, "library_roots");
    const QStringList folders = ListSetting(config, "mira_folders");
    mira_folder_ = folders.isEmpty() ? QDir::homePath() + "/Mira" : folders.front();
    FillMenuDetails();
  });
  api::GetDesktopEntryCandidatesAsync(this, [this](DesktopEntryCandidatesResult result) {
    candidates_ = std::move(result);
    FillMenuDetails();
  });
}

void AddGamesCard::ShowPage(SettingsCard* page) {
  if (page_ != nullptr) page_->deleteLater();
  page_ = page;
  page->setParent(this);
  layout_->addWidget(page);
  page->show();
  setFocus();
}

void AddGamesCard::Close() {
  cards::Close(this);
}

QString AddGamesCard::MoveTarget(bool app) const {
  return mira_folder_ + (app ? "/Applications" : "/Games");
}

bool AddGamesCard::InLibraryRoot(const QString& path) const {
  const QString clean = QDir::cleanPath(path);
  for (const QString& root : library_roots_) {
    const QString base = QDir::cleanPath(root);
    if (clean == base || clean.startsWith(base + "/")) return true;
  }
  return false;
}

void AddGamesCard::ShowMenu() {
  ++request_;  // a pick still being read no longer opens its page
  name_hint_.clear();
  SettingsCard* page = NewPage();
  AddCardHeading(page, "Add games", "Close", [this] { Close(); });

  const auto add_row = [page](icons::Glyph glyph, const QString& label, const QString& detail,
                              const QString& button) {
    auto* row = new SettingRow(label, QString(), page);
    row->SetLeading(Tile(row, glyph));
    QLabel* line = Detail(row, detail);
    row->SetBelow(line);
    auto* choose = new QPushButton(button, row);
    row->AddControl(choose);
    page->AddRow(row);
    return std::pair{line, choose};
  };

  auto [scan_line, scan] = add_row(icons::Glyph::Refresh, "Scan library folders", "…", "Scan");
  scan_detail_ = scan_line;
  connect(scan, &QPushButton::clicked, this, [this] {
    actions::ScanLibrary(window());
    Close();
  });

  QPushButton* installed = add_row(icons::Glyph::Folder, "Already installed",
                                   "A game or app's folder, or the program itself", "Choose…")
                               .second;
  auto* pick = new QMenu(installed);
  pick->addAction("Folder…", this, [this] {
    const QString folder =
        QFileDialog::getExistingDirectory(this, "The game or app's folder", QDir::homePath());
    if (!folder.isEmpty()) Classify(folder, false);
  });
  pick->addAction("Program…", this, [this] {
    const QString file =
        QFileDialog::getOpenFileName(this, "The program that starts it", QDir::homePath());
    if (!file.isEmpty()) Classify(file, false);
  });
  installed->setMenu(pick);

  QPushButton* installer =
      add_row(icons::Glyph::Download, "Installer", "Adds the game and installs it", "Choose…")
          .second;
  connect(installer, &QPushButton::clicked, this, [this] {
    const QString file = QFileDialog::getOpenFileName(
        this, "Installer", QDir::homePath(), "Installers (*.exe *.msi *.EXE *.MSI);;All files (*)");
    if (!file.isEmpty()) Classify(file, true);
  });

  auto [menu_line, menu] = add_row(icons::Glyph::Grid, "From your app menu", "…", "Choose…");
  menu_detail_ = menu_line;
  menu_choose_ = menu;
  menu->setEnabled(false);
  connect(menu, &QPushButton::clicked, this, [this] { ShowAppMenu(); });

  ShowPage(page);
  FillMenuDetails();
}

void AddGamesCard::FillMenuDetails() {
  if (scan_detail_ != nullptr && !mira_folder_.isEmpty()) {
    QStringList roots;
    for (const QString& root : library_roots_) roots << HomeRelative(root);
    scan_detail_->setText(roots.isEmpty() ? QString("No library folders") : roots.join(", "));
  }
  if (menu_detail_ != nullptr && candidates_) {
    const std::size_t count = candidates_->candidates.size();
    if (!candidates_->ok) {
      menu_detail_->setText("Could not read the app menu: " +
                            error_help::Describe(candidates_->error));
    } else if (count == 0) {
      menu_detail_->setText("Every entry is already in Mira");
    } else {
      menu_detail_->setText(count == 1 ? QString("1 entry, Flatpaks included")
                                       : QString("%1 entries, Flatpaks included").arg(count));
    }
    menu_choose_->setEnabled(candidates_->ok && count > 0);
  }
}

void AddGamesCard::OpenPath(const QString& path, const QString& name) {
  Classify(path, std::nullopt);
  name_hint_ = name;
}

void AddGamesCard::Classify(const QString& path, std::optional<bool> installer) {
  const int request = ++request_;
  api::ClassifyImportAsync(this, path.toStdString(),
                           [this, path, installer, request](ImportGuessResult guess) {
                             if (request != request_) return;
                             if (!guess.ok) {
                               // The page stays as it is; the line under it says why.
                               if (page_ != nullptr) {
                                 QLabel* error = page_->findChild<QLabel*>("add_error");
                                 if (error == nullptr) {
                                   error = AddErrorLine(page_);
                                   error->setObjectName("add_error");
                                 }
                                 error->setText("Could not read " + QFileInfo(path).fileName() +
                                                ": " + error_help::Describe(guess.error));
                                 error->show();
                               }
                               return;
                             }
                             if (installer.value_or(guess.installer)) {
                               ShowInstaller(path, guess);
                             } else {
                               ShowInstalled(path, guess);
                             }
                           });
}

void AddGamesCard::ShowInstalled(const QString& path, const ImportGuessResult& guess) {
  const bool folder = QFileInfo(path).isDir();
  const QString fallback = folder ? QFileInfo(path).fileName() : QFileInfo(path).completeBaseName();
  const QString guessed = !name_hint_.isEmpty() ? name_hint_
                          : guess.name.empty()  ? fallback
                                                : QString::fromStdString(guess.name);

  SettingsCard* page = NewPage();
  AddCardBack(page, [this] { ShowMenu(); });
  QLabel* heading = AddCardHeading(
      page, guessed, "Close", [this] { Close(); }, QString::fromStdString(guess.reason));

  auto* where = new SettingRow(folder ? "Folder" : "Program", QString(), page);
  auto* path_line = Detail(where, HomeRelative(path));
  path_line->setTextInteractionFlags(Qt::TextSelectableByMouse);
  where->SetBelow(path_line);
  auto* change = new QPushButton("Change…", where);
  connect(change, &QPushButton::clicked, this, [this, folder, path] {
    const QString picked =
        folder ? QFileDialog::getExistingDirectory(this, "The game or app's folder", path)
               : QFileDialog::getOpenFileName(this, "The program that starts it", path);
    if (!picked.isEmpty()) Classify(picked, false);
  });
  where->AddControl(change);
  page->AddRow(where);

  // A folder's programs, best first; a program is its own.
  QComboBox* program = nullptr;
  if (folder) {
    auto* row = new SettingRow("Program that starts it", QString(), page);
    program = new QComboBox(row);
    for (const ImportGuessResult::Program& candidate : guess.programs) {
      program->addItem(QString::fromStdString(candidate.path),
                       QString::fromStdString(candidate.platform));
    }
    program->setEnabled(!guess.programs.empty());
    if (guess.programs.empty()) program->addItem("No programs in this folder");
    row->AddControl(program);
    page->AddRow(row);
  }

  auto* name_row = new SettingRow("Name", QString(), page);
  auto* name = new QLineEdit(guessed, name_row);
  name->setFixedWidth(240);
  name_row->AddControl(name);
  page->AddRow(name_row);

  auto* kind_row = new SettingRow("It's", QString(), page);
  auto* segmented = new QWidget(kind_row);
  segmented->setObjectName("segmented");
  auto* segmented_layout = new QHBoxLayout(segmented);
  segmented_layout->setContentsMargins(3, 3, 3, 3);
  segmented_layout->setSpacing(3);
  auto* kinds = new QButtonGroup(segmented);
  auto* game = new QPushButton("A game", segmented);
  auto* app = new QPushButton("An app", segmented);
  for (QPushButton* button : {game, app}) {
    button->setCheckable(true);
    kinds->addButton(button);
    segmented_layout->addWidget(button);
  }
  (guess.kind == "app" ? app : game)->setChecked(true);
  kind_row->AddControl(segmented);
  page->AddRow(kind_row);

  // Already in a library folder, it has nowhere to move to.
  Switch* move = nullptr;
  if (!InLibraryRoot(path) && !mira_folder_.isEmpty()) {
    auto* row = new SettingRow(QString(), QString(), page);
    move = new Switch(row);
    move->setChecked(true);
    row->AddControl(move);
    page->AddRow(row);
    const auto label = [this, row, app] {
      row->Label()->setText("Move it into " + HomeRelative(MoveTarget(app->isChecked())));
    };
    label();
    connect(kinds, &QButtonGroup::buttonClicked, row, label);
  }

  QLabel* error = AddErrorLine(page);
  error->setObjectName("add_error");
  QHBoxLayout* buttons = AddButtonRow(page);
  auto* cancel = new QPushButton("Cancel", page);
  connect(cancel, &QPushButton::clicked, this, [this] { Close(); });
  buttons->addWidget(cancel);
  auto* add = new QPushButton(page);
  add->setDefault(true);
  buttons->addWidget(add);
  const auto refresh = [add, name, heading, program] {
    const QString text = name->text().trimmed();
    add->setText(text.isEmpty() ? QString("Add") : "Add " + text);
    heading->setText(text.isEmpty() ? QString("Add") : text);
    add->setEnabled(!text.isEmpty() && (program == nullptr || program->isEnabled()));
  };
  refresh();
  connect(name, &QLineEdit::textChanged, add, refresh);

  connect(add, &QPushButton::clicked, this, [=, this] {
    add->setEnabled(false);
    error->hide();
    const bool as_app = app->isChecked();
    const std::string exe = program != nullptr ? program->currentText().toStdString()
                                               : QFileInfo(path).fileName().toStdString();
    const std::string platform =
        program != nullptr
            ? program->currentData().toString().toStdString()
            : (guess.programs.empty() ? std::string() : guess.programs.front().platform);
    const std::string title = name->text().trimmed().toStdString();
    const auto failed = [add, error](const QString& what, const ApiError& why) {
      add->setEnabled(true);
      error->setText(what + error_help::Describe(why));
      error->show();
    };
    // Where it is now: its folder and the program in it, relative to that folder.
    const auto add_at = [this, exe, title, platform, as_app, folder, failed](const QString& at) {
      const std::string install_path =
          folder ? at.toStdString() : QFileInfo(at).absolutePath().toStdString();
      const std::string program_path = folder ? exe : QFileInfo(at).fileName().toStdString();
      api::AddManualGameAsync(this, install_path, program_path, title, platform,
                              /*is_installer=*/false, as_app,
                              [this, failed](GameDetailResult result) {
                                if (result.ok) return Close();
                                failed("Could not add it: ", result.error);
                              });
    };
    if (move == nullptr || !move->isChecked()) return add_at(path);
    api::ImportPathAsync(this, path.toStdString(), as_app ? "app" : "game",
                         [add_at, failed](ImportPathResult moved) {
                           if (!moved.ok) return failed("Could not move it: ", moved.error);
                           add_at(QString::fromStdString(moved.path));
                         });
  });

  ShowPage(page);
  name->setFocus();
}

void AddGamesCard::ShowInstaller(const QString& path, const ImportGuessResult& guess) {
  const InstallerInfoResult& info = guess.installer_info;
  const QString guessed = !name_hint_.isEmpty() ? name_hint_
                          : guess.name.empty()  ? QFileInfo(path).completeBaseName()
                                                : QString::fromStdString(guess.name);

  SettingsCard* page = NewPage();
  AddCardBack(page, [this] { ShowMenu(); });
  QLabel* heading = AddCardHeading(page, QString(), "Close", [this] { Close(); });

  auto* installer = new SettingRow("Installer", QString(), page);
  QStringList parts{QFileInfo(path).fileName()};
  if (info.ok) {
    if (const QString format = InstallerFormatName(info.format); !format.isEmpty()) parts << format;
    parts << SizeText(info.size_bytes);
  }
  auto* file = Detail(installer, parts.join("  ·  "));
  file->setToolTip(path);
  installer->SetBelow(file);
  auto* change = new QPushButton("Change…", installer);
  change->setToolTip("Pick a different installer file");
  connect(change, &QPushButton::clicked, this, [this, path] {
    const QString picked =
        QFileDialog::getOpenFileName(this, "Installer", QFileInfo(path).absolutePath(),
                                     "Installers (*.exe *.msi *.EXE *.MSI);;All files (*)");
    if (!picked.isEmpty()) Classify(picked, true);
  });
  installer->AddControl(change);
  page->AddRow(installer);

  auto* name_row = new SettingRow("Name", QString(), page);
  auto* name = new QLineEdit(guessed, name_row);
  name->setFixedWidth(240);
  name_row->AddControl(name);
  page->AddRow(name_row);

  // A disc or mounted image is full or read-only: the game's prefix goes on a library root's disk.
  QComboBox* target = nullptr;
  if (!info.targets.empty()) {
    auto* row = new SettingRow("Install onto", QString(), page);
    target = new QComboBox(row);
    for (const InstallerInfoResult::Target& choice : info.targets) {
      target->addItem(
          QString("%1  ·  %2 free")
              .arg(HomeRelative(QString::fromStdString(choice.path)), SizeText(choice.free_bytes)),
          QString::fromStdString(choice.path));
    }
    row->AddControl(target);
    page->AddRow(row);
  }

  QLabel* error = AddErrorLine(page);
  error->setObjectName("add_error");
  if (!info.ok) {
    error->setText("Mira can't run this installer itself: add it, then install it from its card.");
    error->show();
  }
  QHBoxLayout* buttons = AddButtonRow(page);
  auto* add_only = new QPushButton("Add without installing", page);
  add_only->setToolTip("Add it as needing install, and install it later");
  buttons->addWidget(add_only);
  auto* quiet = new QPushButton("Install quietly", page);
  quiet->setToolTip("Run the installer in the background without its window");
  KeepSpaceWhenHidden(quiet);
  quiet->setVisible(info.ok && info.silent);
  buttons->addWidget(quiet);
  auto* shown = new QPushButton("Show the installer", page);
  shown->setToolTip("Open the installer's window and click through it yourself");
  shown->setEnabled(info.ok);
  buttons->addWidget(shown);
  (info.ok && info.silent ? quiet : shown)->setDefault(true);

  const auto refresh = [name, heading, add_only, quiet, shown, ok = info.ok] {
    const QString text = name->text().trimmed();
    heading->setText(text.isEmpty() ? QString("Install it?") : "Install " + text + "?");
    add_only->setEnabled(!text.isEmpty());
    quiet->setEnabled(!text.isEmpty());
    shown->setEnabled(ok && !text.isEmpty());
  };
  refresh();
  connect(name, &QLineEdit::textChanged, page, refresh);

  // Added once: a failed install after that retries the install, not the add.
  auto added = std::make_shared<std::string>();
  const auto run = [=, this](std::optional<bool> interactive) {
    for (QPushButton* button : {add_only, quiet, shown}) button->setEnabled(false);
    error->hide();
    const auto failed = [=](const QString& what, const ApiError& why) {
      refresh();
      error->setText(what + error_help::Describe(why));
      error->show();
    };
    const std::string chosen =
        target != nullptr ? target->currentData().toString().toStdString() : std::string();
    const auto install = [=, this](const std::string& id) {
      if (!interactive) return Close();
      api::InstallGameAsync(this, id, *interactive, std::string(), chosen,
                            [this, failed](GameActionResult result) {
                              if (result.ok) return Close();
                              failed("Could not start the install: ", result.error);
                            });
    };
    if (!added->empty()) return install(*added);
    const std::string platform =
        guess.programs.empty() ? std::string() : guess.programs.front().platform;
    api::AddManualGameAsync(
        this, QFileInfo(path).absolutePath().toStdString(),
        QFileInfo(path).fileName().toStdString(), name->text().trimmed().toStdString(), platform,
        /*is_installer=*/true, /*app=*/false, [added, install, failed](GameDetailResult result) {
          if (!result.ok) return failed("Could not add it: ", result.error);
          *added = result.game.id;
          install(*added);
        });
  };
  connect(add_only, &QPushButton::clicked, this, [run] { run(std::nullopt); });
  connect(quiet, &QPushButton::clicked, this, [run] { run(false); });
  connect(shown, &QPushButton::clicked, this, [run] { run(true); });

  ShowPage(page);
}

void AddGamesCard::ShowAppMenu() {
  if (!candidates_ || !candidates_->ok) return;
  SettingsCard* page = NewPage();
  AddCardBack(page, [this] { ShowMenu(); });
  AddCardHeading(page, "From your app menu", "Close", [this] { Close(); });
  auto* search = new QLineEdit(page);
  search->setPlaceholderText("Search");
  search->setClearButtonEnabled(true);
  search->setFixedWidth(170);
  page->Header()->insertWidget(page->Header()->count() - 1, search, 0, Qt::AlignVCenter);

  struct Entry {
    std::string id;
    QString name;
    QWidget* row;
    QCheckBox* pick;
    QPushButton* app;
  };
  auto entries = std::make_shared<std::vector<Entry>>();
  QLabel* count = nullptr;
  QPushButton* add = nullptr;
  std::function<void()> recount;
  for (const DesktopEntryCandidate& candidate : candidates_->candidates) {
    const QString label =
        QString::fromStdString(candidate.name.empty() ? candidate.id : candidate.name);
    auto* row = new SettingRow(label, QString(), page);
    auto* icon = new QLabel(row);
    icon->setFixedSize(kEntryIcon, kEntryIcon);
    icon->setPixmap(EntryIcon(candidate.icon, devicePixelRatioF()));
    row->SetLeading(icon);
    if (candidate.flatpak) {
      auto* tag = new QLabel("Flatpak", row);
      tag->setObjectName("default_tag");
      row->AddAfterLabel(tag);
    } else if (!candidate.command.empty()) {
      auto* command = new ElidedLabel(QString::fromStdString(candidate.command), row);
      command->setProperty("role", "muted");
      row->SetBelow(command);
    }
    // Shown once the entry is ticked, keeping its place so nothing shifts.
    auto* app = new QPushButton("App", row);
    app->setObjectName("chip");
    app->setCheckable(true);
    app->setChecked(candidate.app);
    app->setToolTip("Add it as an app rather than a game");
    KeepSpaceWhenHidden(app);
    app->hide();
    row->AddControl(app);
    auto* pick = new QCheckBox(row);
    pick->setAccessibleName(label);
    row->AddControl(pick);
    page->AddRow(row);
    entries->push_back({candidate.id, label, row, pick, app});
  }
  if (entries->size() > 6) page->SetScrollable();

  QLabel* error = AddErrorLine(page);
  QHBoxLayout* buttons = AddButtonRow(page);
  count = new QLabel(page);
  count->setProperty("role", "subtle");
  buttons->insertWidget(0, count);
  auto* cancel = new QPushButton("Cancel", page);
  connect(cancel, &QPushButton::clicked, this, [this] { Close(); });
  buttons->addWidget(cancel);
  add = new QPushButton("Add", page);
  add->setDefault(true);
  buttons->addWidget(add);

  recount = [entries, count, add] {
    int games = 0;
    int apps = 0;
    for (const Entry& entry : *entries) {
      entry.app->setVisible(entry.pick->isChecked());
      if (entry.pick->isChecked()) ++(entry.app->isChecked() ? apps : games);
    }
    QStringList parts;
    if (games > 0) parts << (games == 1 ? QString("1 game") : QString("%1 games").arg(games));
    if (apps > 0) parts << (apps == 1 ? QString("1 app") : QString("%1 apps").arg(apps));
    count->setText(parts.join(", "));
    add->setText(games + apps > 0 ? QString("Add %1").arg(games + apps) : QString("Add"));
    add->setEnabled(games + apps > 0);
  };
  for (const Entry& entry : *entries) {
    connect(entry.pick, &QCheckBox::toggled, add, recount);
    connect(entry.app, &QPushButton::toggled, add, recount);
  }
  recount();

  connect(search, &QLineEdit::textChanged, page, [entries](const QString& text) {
    for (const Entry& entry : *entries)
      entry.row->setVisible(text.isEmpty() || entry.name.contains(text, Qt::CaseInsensitive));
  });

  connect(add, &QPushButton::clicked, this, [this, entries, add, error] {
    std::vector<std::string> ids;
    std::vector<std::string> apps;
    for (const Entry& entry : *entries) {
      if (!entry.pick->isChecked()) continue;
      ids.push_back(entry.id);
      if (entry.app->isChecked()) apps.push_back(entry.id);
    }
    add->setEnabled(false);
    error->hide();
    api::ImportDesktopEntriesAsync(
        this, ids, apps, [this, add, error](DesktopEntryImportResult result) {
          if (result.ok) return Close();
          add->setEnabled(true);
          error->setText("Could not add them: " + error_help::Describe(result.error));
          error->show();
        });
  });

  ShowPage(page);
  search->setFocus();
}

}  // namespace mira_gui
