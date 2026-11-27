#include "AddGamesCard.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStorageInfo>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <vector>

#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/JsonMapping.h"
#include "../client/api/Config.h"
#include "../client/api/Downloads.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../components/Badge.h"
#include "../components/Button.h"
#include "../components/Chip.h"
#include "../components/FlatCombo.h"
#include "../components/Format.h"
#include "../components/Labels.h"
#include "../components/Metrics.h"
#include "../components/SearchField.h"
#include "../components/Segmented.h"
#include "../components/SettingRow.h"
#include "../components/SheetCard.h"
#include "../components/Switch.h"
#include "../components/WindowCards.h"
#include "../downloads/LinkText.h"
#include "../system/PackageInstall.h"
#include "../theme/Icons.h"
#include "LibraryActions.h"

namespace mira_gui {
namespace {

constexpr int kWidth = 560;
constexpr int kTile = 30;
constexpr int kEntryIcon = 32;

QStringList ListSetting(const ConfigResult& config, const std::string& key) {
  QStringList list;
  if (const auto it = config.values.find(key); it != config.values.end()) {
    for (const std::string& item : mapping::ParseListText(it->second))
      list << ExpandHome(QString::fromStdString(item));
  }
  return list;
}

// The glyph on a tile before each of the first page's rows.
QLabel* Tile(QWidget* parent, icons::Glyph glyph) {
  auto* tile = new QLabel(parent);
  tile->setObjectName("tile_icon");
  tile->setFixedSize(kTile, kTile);
  tile->setAlignment(Qt::AlignCenter);
  icons::Follow(tile, glyph, 18, &theme::Tokens::accent);
  return tile;
}

// One line of muted text under a row's label, cut short rather than wrapped, so the card keeps its
// height as the line fills in.
QLabel* Detail(QWidget* parent, const QString& text) {
  auto* label = new ElidedLabel(text, parent);
  SetTextStyle(label, text::kDetail);
  return label;
}

SheetCard* NewPage() {
  auto* page = new SheetCard();
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

// Enter in a links box clicks `button`; Shift+Enter still starts a new line.
class EnterClicks : public QObject {
 public:
  EnterClicks(QWidget* box, QPushButton* button) : QObject(box), button_(button) {
    box->installEventFilter(this);
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() != QEvent::KeyPress) return QObject::eventFilter(watched, event);
    const auto* key = static_cast<QKeyEvent*>(event);
    if ((key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) ||
        (key->modifiers() & Qt::ShiftModifier))
      return false;
    if (button_->isEnabled()) button_->click();
    return true;
  }

 private:
  QPushButton* button_;
};

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
    mira_folders_.clear();
    for (const QString& folder : folders) mira_folders_ << QDir::cleanPath(folder);
    // As mirad picks: the setting, else the one in the home folder.
    const auto it = config.values.find("default_mira_folder");
    const QString chosen = it != config.values.end()
                               ? QDir::cleanPath(ExpandHome(QString::fromStdString(it->second)))
                               : QString();
    default_folder_ = mira_folders_.contains(chosen) ? chosen : QString();
    for (const QString& folder : mira_folders_) {
      if (default_folder_.isEmpty() && (folder + "/").startsWith(QDir::homePath() + "/"))
        default_folder_ = folder;
    }
    FillMenuDetails();
  });
  api::GetDesktopEntryCandidatesAsync(this, [this](DesktopEntryCandidatesResult result) {
    candidates_ = std::move(result);
    FillMenuDetails();
  });
}

void AddGamesCard::ShowPage(SheetCard* page) {
  if (page_ != nullptr) {
    // Out of the layout now: until it's deleted, its height would add to the new page's.
    layout_->removeWidget(page_);
    page_->hide();
    page_->deleteLater();
  }
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
  SheetCard* page = NewPage();
  page->SetHeading("Add games");
  page->SetClose("Close", [this] { Close(); });

  std::vector<QPushButton*> buttons;
  const auto add_row = [page, &buttons](icons::Glyph glyph, const QString& label,
                                        const QString& detail, const QString& button) {
    auto* row = new SettingRow(label, QString(), page);
    row->SetLeading(Tile(row, glyph));
    QLabel* line = Detail(row, detail);
    row->SetBelow(line);
    auto* choose = new QPushButton(button, row);
    buttons.push_back(choose);
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

  // A file picker opens for a folder or a file, never both: one button for each.
  QPushButton* folder = nullptr;
  QPushButton* program = nullptr;
  {
    auto* row = new SettingRow("Already installed", QString(), page);
    row->SetLeading(Tile(row, icons::Glyph::Folder));
    row->SetBelow(Detail(row, "A game or app's folder for Mira to search, or the program itself"));
    folder = new QPushButton("Folder", row);
    program = new QPushButton("Program", row);
    row->AddControl(folder);
    row->AddControl(program);
    page->AddRow(row);
  }
  connect(folder, &QPushButton::clicked, this, [this] {
    const QString picked =
        QFileDialog::getExistingDirectory(this, "The game or app's folder", QDir::homePath());
    if (!picked.isEmpty()) Classify(picked, false);
  });
  connect(program, &QPushButton::clicked, this, [this] {
    const QString file =
        QFileDialog::getOpenFileName(this, "The program that starts it", QDir::homePath());
    if (!file.isEmpty()) Classify(file, false);
  });

  QPushButton* installer =
      add_row(icons::Glyph::Download, "Installer", "Adds the game and installs it", "Choose…")
          .second;
  connect(installer, &QPushButton::clicked, this, [this] {
    const QString file = QFileDialog::getOpenFileName(
        this, "Installer", QDir::homePath(), "Installers (*.exe *.msi *.EXE *.MSI);;All files (*)");
    if (!file.isEmpty()) Classify(file, true);
  });

  QPushButton* links = add_row(icons::Glyph::External, "Download from links",
                               "Downloads, extracts and installs one game", "Paste…")
                           .second;
  connect(links, &QPushButton::clicked, this, [this] { ShowLinks(); });

  auto [menu_line, menu] = add_row(icons::Glyph::Grid, "From your app menu", "…", "Choose…");
  menu_detail_ = menu_line;
  menu_choose_ = menu;
  menu->setEnabled(false);
  connect(menu, &QPushButton::clicked, this, [this] { ShowAppMenu(); });
  // One width, so the buttons line up down the card.
  int widest = 0;
  buttons.push_back(folder);
  buttons.push_back(program);
  for (QPushButton* button : buttons) widest = std::max(widest, button->sizeHint().width());
  for (QPushButton* button : buttons) button->setMinimumWidth(widest);

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
      menu_detail_->setText(Counted(count, "entry", "entries") + ", Flatpaks included");
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
  api::ClassifyImportAsync(
      this, path.toStdString(), [this, path, installer, request](ImportGuessResult guess) {
        if (request != request_) return;
        if (!guess.ok)
          return Fail("Could not read " + QFileInfo(path).fileName() + ": ", guess.error);
        if (installer.value_or(guess.installer)) {
          AddInstaller(path, guess);
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

  SheetCard* page = NewPage();
  page->SetBack([this] { ShowMenu(); });
  QLabel* heading = page->SetHeading(guessed, QString::fromStdString(guess.reason));
  page->SetClose("Close", [this] { Close(); });

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
    program = new FlatCombo(row);
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
  auto* kinds = new Segmented({{"game", "A game"}, {"app", "An app"}}, kind_row);
  kinds->SetValue(guess.kind == "app" ? "app" : "game");
  QPushButton* app = kinds->Option("app");
  kind_row->AddControl(kinds);
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
    connect(kinds, &Segmented::Changed, row, label);
  }

  QLabel* error = page->ErrorLine();
  QHBoxLayout* buttons = page->ButtonRow();
  page->AddCancel([this] { Close(); });
  auto* add = new Button(QString(), ButtonKind::Primary, page);
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

// Added as not set up; its setup card asks the rest, once this one is gone.
void AddGamesCard::AddInstaller(const QString& path, const ImportGuessResult& guess) {
  const QString name = !name_hint_.isEmpty() ? name_hint_
                       : guess.name.empty()  ? QFileInfo(path).completeBaseName()
                                             : QString::fromStdString(guess.name);
  // A folder led by an installer, or the installer itself.
  const QFileInfo file(path);
  const bool folder = file.isDir() && !guess.programs.empty();
  const std::string install_path = (folder ? path : file.absolutePath()).toStdString();
  const std::string program = folder ? guess.programs.front().path : file.fileName().toStdString();
  const std::string platform =
      guess.programs.empty() ? std::string() : guess.programs.front().platform;
  api::AddManualGameAsync(this, install_path, program, name.toStdString(), platform,
                          /*is_installer=*/true, /*app=*/false, [this](GameDetailResult result) {
                            if (!result.ok) return Fail("Could not add it: ", result.error);
                            emit SetupRequested(QString::fromStdString(result.game.id));
                            Close();
                          });
}

// The page stays as it is; the line under it says why. Opened by a drop, there is none.
void AddGamesCard::Fail(const QString& what, const ApiError& why) {
  if (page_ == nullptr) {
    notify::FailedRequest(window(), what.chopped(2), why);
    return Close();
  }
  QLabel* error = page_->ErrorLine();
  error->setText(what + error_help::Describe(why));
  error->show();
}

void AddGamesCard::ShowLinks() {
  const int request = ++request_;
  SheetCard* page = NewPage();
  page->SetBack([this] { ShowMenu(); });
  page->SetHeading("Download from links");
  page->SetClose("Close", [this] { Close(); });

  auto* intro = new SettingRow("Links for one game", QString(), page);
  intro->SetBelow(Detail(intro, "One per line. Enter downloads, Shift+Enter adds a line."));
  page->AddRow(intro);
  auto* text = new QPlainTextEdit(page);
  text->setFixedHeight(120);
  text->setPlaceholderText("https://…");
  // What was just copied is most likely what's wanted.
  const std::vector<std::string> copied = downloads::ParseLinks(QApplication::clipboard()->text());
  for (const std::string& link : copied) text->appendPlainText(QString::fromStdString(link));
  auto* box = new QWidget(page);
  auto* box_layout = new QVBoxLayout(box);
  box_layout->setContentsMargins(kRowPaddingX, 0, kRowPaddingX, 10);
  box_layout->addWidget(text);
  page->AddRow(box);

  auto* name_row = new SettingRow("Name", QString(), page);
  auto* name = new QLineEdit(name_row);
  name->setPlaceholderText("From the download's name");
  name->setFixedWidth(240);
  name_row->AddControl(name);
  page->AddRow(name_row);

  auto* password_row = new SettingRow("Archive password", QString(), page);
  auto* password = new QLineEdit(password_row);
  password->setPlaceholderText("Only if it has one");
  password->setFixedWidth(240);
  password_row->AddControl(password);
  page->AddRow(password_row);

  // One Mira folder per drive: which one it goes onto.
  QComboBox* target = nullptr;
  if (mira_folders_.size() > 1) {
    auto* row = new SettingRow("Download to", QString(), page);
    target = new FlatCombo(row);
    for (const QString& folder : mira_folders_) {
      const qint64 free = QStorageInfo(folder).bytesAvailable();
      target->addItem(free < 0
                          ? HomeRelative(folder)
                          : QString("%1  ·  %2 free").arg(HomeRelative(folder), SizeText(free)),
                      folder);
      if (folder == default_folder_) target->setCurrentIndex(target->count() - 1);
    }
    row->AddControl(target);
    page->AddRow(row);
  }

  QLabel* error = page->ErrorLine();
  QHBoxLayout* buttons = page->ButtonRow();
  page->AddCancel([this] { Close(); });
  auto* start = new Button("Download", ButtonKind::Primary, page);
  start->setDefault(true);
  buttons->addWidget(start);
  const auto refresh = [text, start] {
    start->setEnabled(!downloads::ParseLinks(text->toPlainText()).empty());
  };
  refresh();
  connect(text, &QPlainTextEdit::textChanged, page, refresh);
  new EnterClicks(text, start);
  for (QLineEdit* field : {name, password}) {
    connect(field, &QLineEdit::returnPressed, start, [start] {
      if (start->isEnabled()) start->click();
    });
  }
  connect(start, &QPushButton::clicked, this, [=, this] {
    const std::vector<std::string> links = downloads::ParseLinks(text->toPlainText());
    const std::string folder =
        target != nullptr ? target->currentData().toString().toStdString() : std::string();
    const auto again = [=](const QString& why) {
      start->setText("Download");
      start->setEnabled(true);
      if (why.isEmpty()) return;
      error->setText(why);
      error->show();
    };
    start->setEnabled(false);
    start->setText("Checking…");
    error->hide();
    // Read first, so a dead link is said here rather than failing in Downloads later.
    const auto add = [=, this] {
      api::CheckLinksAsync(this, links, [=, this](LinkCheckResult checked) {
        if (request != request_) return;
        if (!checked.ok) return again(error_help::Describe(checked.error));
        api::AddDownloadAsync(this, name->text().trimmed().toStdString(), links,
                              password->text().toStdString(), folder,
                              [=, this](StoreActionResult result) {
                                if (!result.ok) return again(error_help::Describe(result.error));
                                emit DownloadStarted();
                                Close();
                              });
      });
    };
    // aria2 downloads and 7-Zip extracts: offered for install here, like any other tool Mira needs.
    system::EnsurePackages(this, "downloads", "Downloading from links", [=, this](bool ready) {
      if (!ready || request != request_) return again(QString());
      system::EnsurePackages(this, "archives", "Downloading from links", [=](bool) { add(); });
    });
  });
  ShowPage(page);
  text->setFocus();
}

void AddGamesCard::ShowAppMenu() {
  if (!candidates_ || !candidates_->ok) return;
  SheetCard* page = NewPage();
  page->SetBack([this] { ShowMenu(); });
  page->SetHeading("From your app menu");
  page->SetClose("Close", [this] { Close(); });
  auto* search = new SearchField("Search", page);
  search->setFixedWidth(170);
  page->AddHeaderWidget(search);

  struct Entry {
    std::string id;
    QString name;
    QWidget* row;
    QCheckBox* pick;
    Chip* app;
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
      row->AddAfterLabel(new Badge("Flatpak", Tone::Accent, row));
    } else if (!candidate.command.empty()) {
      auto* command = new ElidedLabel(QString::fromStdString(candidate.command), row);
      SetTextStyle(command, text::kDetail);
      row->SetBelow(command);
    }
    // Shown once the entry is ticked, keeping its place so nothing shifts.
    auto* app = new Chip("App", row);
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

  QLabel* error = page->ErrorLine();
  QHBoxLayout* buttons = page->ButtonRow();
  count = new QLabel(page);
  SetTextStyle(count, text::kSubtle);
  buttons->insertWidget(0, count);
  page->AddCancel([this] { Close(); });
  add = new Button("Add", ButtonKind::Primary, page);
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
    if (games > 0) parts << Counted(games, "game");
    if (apps > 0) parts << Counted(apps, "app");
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
