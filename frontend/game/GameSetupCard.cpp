#include "GameSetupCard.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <ranges>

#include "../app/ErrorHelp.h"
#include "../client/api/Artwork.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../client/api/Stores.h"
#include "../components/Badge.h"
#include "../components/Button.h"
#include "../components/FlatCombo.h"
#include "../components/Format.h"
#include "../components/Labels.h"
#include "../components/Metrics.h"
#include "../components/PathField.h"
#include "../components/SearchField.h"
#include "../components/Segmented.h"
#include "../components/SettingRow.h"
#include "../components/SheetCard.h"
#include "../components/Switch.h"
#include "../components/Text.h"
#include "../library/ArtworkStore.h"
#include "../theme/Icons.h"
#include "CardParts.h"

namespace mira_gui {
namespace {

constexpr int kWidth = 620;
constexpr int kLabelWidth = 120;
constexpr int kGlyphTile = 32;
constexpr QSize kCover(46, 62);
constexpr QSize kResultCover(40, 54);
constexpr int kRowTextInset = 6;  // a row's label starts past the dot
constexpr int kMaxTitles = 5;
const QString kDot = "  ·  ";

// The label column every row's field starts after.
SettingRow* FieldRow(SheetCard* page, const QString& label) {
  auto* row = new SettingRow(label, QString(), page);
  row->Label()->setFixedWidth(kLabelWidth);
  row->DropUndo();
  return row;
}

QLabel* Hint(QWidget* parent, const QString& text = {}) {
  auto* label = new QLabel(text, parent);
  SetTextStyle(label, text::kDetail);
  label->setWordWrap(true);
  return label;
}

// Two lines in place of a row's label: a name and what's known about it.
QLabel* AddTwoLines(SettingRow* row, QLabel* first) {
  row->Label()->hide();
  row->DropUndo();
  auto* lines = new QWidget(row);
  auto* layout = new QVBoxLayout(lines);
  layout->setContentsMargins(kRowTextInset, 0, 0, 0);
  layout->setSpacing(2);
  first->setParent(lines);
  SetTextStyle(first, text::kSection);
  layout->addWidget(first);
  auto* second = new ElidedLabel(QString(), lines);
  SetTextStyle(second, text::kDetail);
  layout->addWidget(second);
  row->AddWideControl(lines);
  return second;
}

bool IsGenericName(const QString& name) {
  const QString lower = name.trimmed().toLower();
  return lower.startsWith("new folder") ||
         QStringList{"game", "games", "setup", "installer", "bin", "data", "untitled folder"}
             .contains(lower);
}

QString Comparable(const QString& name) {
  QString out;
  for (const QChar c : name.toLower())
    if (c.isLetterOrNumber()) out += c;
  return out;
}

// Has mirad fetch the covers it doesn't have yet.
void QueueCovers(QWidget* context, const std::vector<TitleMatch>& matches) {
  std::vector<StoreTitle> titles;
  for (const TitleMatch& match : matches)
    titles.push_back({.ref = std::to_string(match.appid), .title = match.name});
  api::QueueTitleArtworkAsync(context, "steam", std::move(titles), {});
}

// As Windows programs see it: what follows the prefix's drive_c.
QString WindowsPath(const QString& path) {
  const qsizetype at = path.indexOf("/drive_c/");
  return at < 0 ? HomeRelative(path) : "C:" + path.mid(at + 8).replace('/', '\\');
}

}  // namespace

GameSetupCard::GameSetupCard(const GameDetail& game, const InstallerInfoResult& installer,
                             ArtworkStore* artwork, QWidget* parent)
    : QWidget(parent), game_(game), installer_(installer), artwork_(artwork) {
  installed_ = game.setup == "confirm" && !game.data_dir.empty() &&
               game.install_path.starts_with(game.data_dir + "/");
  appid_ = QString::fromStdString(game.identity.steam_appid).toLongLong();
  title_name_ =
      QString::fromStdString(game.identity.title.empty() ? game.name : game.identity.title);
  const QFileInfo folder(QString::fromStdString(game.install_path));
  Build(installed_ ? "Installed into " + WindowsPath(folder.filePath())
                   : "New in " + HomeRelative(folder.absolutePath()) + kDot + folder.fileName());
  name_->setText(QString::fromStdString(game.name));
  kind_->SetValue(std::ranges::contains(game.tags, tags::kApp) ? "app" : "game");
  SetPrograms(GamePrograms());
  ShowTitle();
  LoadFacts();
  Refresh();
}

void GameSetupCard::Identify() {
  api::GetGameAsync(this, game_.id, [this](GameDetailResult result) {
    if (!result.ok || picked_appid_) return;
    const std::int64_t appid =
        QString::fromStdString(result.game.identity.steam_appid).toLongLong();
    if (appid <= 0) return;
    appid_ = appid;
    if (!result.game.identity.title.empty())
      title_name_ = QString::fromStdString(result.game.identity.title);
    ShowTitle();
    LoadFacts();
  });
}

// Its year and developer, once its store info is fetched.
void GameSetupCard::LoadFacts() {
  api::GetMetadataAsync(this, game_.id, [this](GameMetadataResult result) {
    if (!result.ok || result.missing || picked_appid_) return;
    QStringList facts;
    static const QRegularExpression year("\\b(19|20)\\d\\d\\b");
    if (const auto match = year.match(QString::fromStdString(result.metadata.release_date));
        match.hasMatch())
      facts << match.captured();
    if (!result.metadata.developers.empty())
      facts << QString::fromStdString(result.metadata.developers.front());
    title_facts_ = facts.join(kDot);
    ShowTitle();
  });
}

GameSetupCard::GameSetupCard(const ArchiveContentsResult& archive, ArtworkStore* artwork,
                             QWidget* parent)
    : QWidget(parent), archive_(archive), artwork_(artwork) {
  const QString folder =
      HomeRelative(QFileInfo(QString::fromStdString(archive.archive.path)).absolutePath());
  Build("An archive arrived in " + folder + ". Nothing is extracted until you say so.");
  name_->setText(QString::fromStdString(archive.suggested_name));
  kind_->SetValue("game");
  ShowArchive(archive);
}

SheetCard* GameSetupCard::NewPage() {
  auto* page = new SheetCard(QString(), this);
  page->setFixedWidth(kWidth);
  layout_->addWidget(page);
  return page;
}

void GameSetupCard::Build(const QString& subtitle) {
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  page_ = NewPage();
  heading_ = page_->SetHeading(QString(), subtitle);
  page_->SetClose(IsArchive() ? "Ask later" : "Set it up later", [this] { emit CloseRequested(); });

  if (game_.setup == "failed" || (game_.setup == "install" && !game_.last_error.empty())) {
    problem_ = new QLabel(game_.last_error.empty() ? QString("Mira couldn't set it up.")
                                                   : QString::fromStdString(game_.last_error),
                          page_);
    problem_->setObjectName("problem_box");
    problem_->setWordWrap(true);
    auto* around = new QWidget(page_);
    auto* box = new QVBoxLayout(around);
    box->setContentsMargins(kRowPaddingX, 4, kRowPaddingX, 12);
    box->addWidget(problem_);
    page_->AddRow(around);
  }
  if (IsArchive()) AddArchiveRows();
  AddTitleRow();

  auto* name_row = FieldRow(page_, "Name");
  name_ = new QLineEdit(name_row);
  name_row->AddWideControl(name_);
  generic_ = new Badge("Generic name", Tone::Warning, name_row);
  generic_->setToolTip("Too generic to find its title by");
  name_row->AddAfterLabel(generic_);
  page_->AddRow(name_row);
  connect(name_, &QLineEdit::textChanged, this, &GameSetupCard::Refresh);
  connect(name_, &QLineEdit::editingFinished, this, &GameSetupCard::IdentifyByName);

  AddProgramRow();

  auto* does_row = FieldRow(page_, "This program");
  does_ = new Segmented({{"starts", "Starts it"}, {"installs", "Installs it"}}, does_row);
  does_->SetValue("starts");
  does_row->AddAfterLabel(does_);
  does_hint_ = Hint(does_row, "Set from its name. Change it if this is the game itself.");
  does_row->AddWideControl(does_hint_);
  page_->AddRow(does_row);
  connect(does_, &Segmented::Changed, this, [this] {
    picked_kind_ = true;
    if (Installs()) LoadInstaller();
    Refresh();
  });

  AddInstallRows();

  auto* kind_row = FieldRow(page_, "It's");
  kind_ = new Segmented({{"game", "A game"}, {"app", "An app"}}, kind_row);
  kind_row->AddAfterLabel(kind_);
  page_->AddRow(kind_row);

  if (IsArchive()) {
    auto* row = new SettingRow("Delete the archive afterwards", QString(), page_);
    delete_archive_ = new Switch(row);
    delete_archive_->setChecked(true);
    row->AddControl(delete_archive_);
    page_->AddRow(row);
  }
  AddButtons();
}

void GameSetupCard::AddArchiveRows() {
  auto* row = new SettingRow(QString(), QString(), page_);
  auto* glyph = new QLabel(row);
  glyph->setObjectName("tile_icon");
  glyph->setFixedSize(kGlyphTile, kGlyphTile);
  glyph->setAlignment(Qt::AlignCenter);
  icons::Follow(glyph, icons::Glyph::Layers, 18, &theme::Tokens::accent);
  row->SetLeading(glyph);
  auto* name = new ElidedLabel(QString::fromStdString(archive_.archive.name), row);
  name->setToolTip(QString::fromStdString(archive_.archive.path));
  archive_facts_ = AddTwoLines(row, name);
  page_->AddRow(row);

  password_row_ = FieldRow(page_, "Password");
  password_edit_ = new QLineEdit(password_row_);
  password_edit_->setPlaceholderText("Saved for next time once it works");
  password_row_->AddWideControl(password_edit_);
  auto* open = new QPushButton("Open", password_row_);
  open->setEnabled(false);
  connect(password_edit_, &QLineEdit::textChanged, open,
          [open](const QString& text) { open->setEnabled(!text.isEmpty()); });
  connect(password_edit_, &QLineEdit::returnPressed, this, &GameSetupCard::OpenWithPassword);
  connect(open, &QPushButton::clicked, this, &GameSetupCard::OpenWithPassword);
  password_row_->AddControl(open);
  page_->AddRow(password_row_);
}

void GameSetupCard::AddTitleRow() {
  title_row_ = new SettingRow(QString(), QString(), page_);
  cover_ = new QLabel(title_row_);
  cover_->setFixedSize(kCover);
  title_row_->SetLeading(cover_);
  auto* name = new ElidedLabel(QString(), title_row_);
  name->setObjectName("title_name");
  title_line_ = AddTwoLines(title_row_, name);
  change_title_ = new QPushButton("Change…", title_row_);
  connect(change_title_, &QPushButton::clicked, this, &GameSetupCard::OpenTitles);
  title_row_->AddControl(change_title_);
  page_->AddRow(title_row_);
  connect(artwork_, &ArtworkStore::CoverChanged, this, [this](const QString& id) {
    // A game this new is still being looked up; its cover comes with its title.
    if (id == QString::fromStdString(game_.id) && appid_ <= 0 && !picked_appid_) Identify();
    if (id == QString::fromStdString(game_.id) || id == QString("steam-%1").arg(appid_))
      ShowTitle();
  });
}

void GameSetupCard::AddProgramRow() {
  auto* row = FieldRow(page_, "Program");
  if (installed_) {
    auto* list = new QWidget(row);
    program_list_layout_ = new QVBoxLayout(list);
    program_list_layout_->setContentsMargins(0, 2, 0, 2);
    program_list_layout_->setSpacing(6);
    program_list_ = new QButtonGroup(this);
    row->AddWideControl(list);
  } else {
    program_ = new QComboBox(row);
    program_->setProperty("boxed", true);
    program_->setMinimumContentsLength(12);
    program_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    row->AddWideControl(program_);
    connect(program_, &QComboBox::activated, this, [this] {
      picked_kind_ = false;
      if (Installs()) LoadInstaller();
      Refresh();
    });
  }
  if (!IsArchive()) {
    auto* browse = new QPushButton("Browse…", row);
    connect(browse, &QPushButton::clicked, this, &GameSetupCard::Browse);
    row->AddControl(browse);
  }
  page_->AddRow(row);
}

void GameSetupCard::AddInstallRows() {
  installer_row_ = FieldRow(page_, "Installer");
  installer_line_ = Hint(installer_row_);
  installer_row_->AddWideControl(installer_line_);
  page_->AddRow(installer_row_);
  target_row_ = FieldRow(page_, "Install onto");
  target_ = new QComboBox(target_row_);
  target_->setProperty("boxed", true);
  target_row_->AddWideControl(target_);
  page_->AddRow(target_row_);
}

void GameSetupCard::AddButtons() {
  error_ = page_->ErrorLine();
  QHBoxLayout* buttons = page_->ButtonRow();
  int at = 0;  // before the row's stretch
  if (IsArchive()) {
    other_ = new QPushButton("Another app is extracting it", page_);
    other_->setToolTip("Leave it alone. Its folder is set up once it's there.");
    connect(other_, &QPushButton::clicked, this, [this] {
      SetBusy(true);
      api::LeaveArchiveAsync(this, archive_.archive.path, [this](GameActionResult result) {
        if (result.ok) return emit CloseRequested();
        SetBusy(false);
        Fail("Could not leave it: ", result.error);
      });
    });
    buttons->insertWidget(at++, other_);
  } else if (game_.setup == "failed") {
    other_ = new QPushButton("Look again", page_);
    other_->setToolTip("Look through its folder again for the program that starts it");
    connect(other_, &QPushButton::clicked, this, &GameSetupCard::LookAgain);
    buttons->insertWidget(at++, other_);
  } else {
    foot_ = Hint(page_, installed_ ? "The other programs can be added as launch options later."
                                   : "Closing it keeps it in the library, marked Not set up.");
    foot_->setWordWrap(false);
    foot_->setIndent(kRowTextInset);
    buttons->insertWidget(at++, foot_);
  }
  shown_ = new Button("Show the installer", ButtonKind::Secondary, page_);
  shown_->setToolTip("Open the installer's window and click through it yourself");
  connect(shown_, &QPushButton::clicked, this, [this] { Confirm(/*interactive=*/true); });
  buttons->addWidget(shown_);
  main_ = new Button(QString(), ButtonKind::Primary, page_);
  main_->setDefault(true);
  connect(main_, &QPushButton::clicked, this, [this] {
    if (IsArchive()) return Extract();
    Confirm(Installs() ? std::optional(!installer_.silent) : std::nullopt);
  });
  buttons->addWidget(main_);
}

void GameSetupCard::ShowTitle() {
  auto* name = title_row_->findChild<QLabel*>("title_name");
  const bool found = appid_ > 0;
  const qreal dpr = devicePixelRatioF();
  cover_->setPixmap(
      found && (picked_appid_ || game_.id.empty())
          ? artwork_->TitleCover("steam", QString::number(appid_), title_name_, kCover, dpr)
          : artwork_->CoverById(QString::fromStdString(game_.id), name_->text(), kCover, dpr,
                                QString::fromStdString(game_.source)));
  name->setText(found ? title_name_ : QString("Not identified"));
  QString line;
  if (found) {
    line = QString("Identified as  ·  Steam %1").arg(appid_);
    if (!title_facts_.isEmpty()) line += kDot + title_facts_;
  } else if (picked_appid_) {
    line = "Uses its name only";
  } else if (IsGenericName(name_->text())) {
    line = "The name is too generic to search";
  } else {
    line = "No title on Steam has this name";
  }
  title_line_->setText(line);
  change_title_->setText(found ? "Change…" : "Choose…");
}

// The title with exactly this name, as mirad itself would take.
void GameSetupCard::IdentifyByName() {
  const QString name = name_->text().trimmed();
  if (!IsArchive() || picked_appid_ || name.isEmpty() || IsGenericName(name)) return ShowTitle();
  const int request = ++request_;
  api::SearchTitlesAsync(this, name.toStdString(),
                         [this, name, request](TitleMatchesResult result) {
                           if (request != request_ || picked_appid_ || !result.ok) return;
                           appid_ = 0;
                           for (const TitleMatch& match : result.matches) {
                             const QString title = QString::fromStdString(match.name);
                             if (Comparable(title) != Comparable(name)) continue;
                             appid_ = match.appid;
                             title_name_ = title;
                             QueueCovers(this, {match});
                             break;
                           }
                           ShowTitle();
                         });
}

void GameSetupCard::OpenTitles() {
  titles_ = NewPage();
  SheetCard* page = titles_;
  page->SetBack([this] { CloseTitles(); });
  page->SetHeading("Which title is it?",
                   "Sets the art, the store page, game fixes and cloud saves.");
  auto* around = new QWidget(page);
  auto* box = new QVBoxLayout(around);
  box->setContentsMargins(kRowPaddingX, 0, kRowPaddingX, 10);
  auto* search = new SearchField("Search titles on Steam", around);
  search->setText(name_->text().trimmed());
  box->addWidget(search);
  page->SetPinnedRow(around);

  const auto use = [this](std::int64_t appid, const QString& name) {
    ++request_;
    picked_appid_ = appid;
    appid_ = appid;
    title_name_ = name;
    title_facts_.clear();
    if (appid > 0 && (IsGenericName(name_->text()) || name_->text().trimmed().isEmpty()))
      name_->setText(name);
    CloseTitles();
    ShowTitle();
  };
  const auto show = [this, page, use](const TitleMatchesResult& result) {
    page->ClearRows();
    if (!result.ok || result.matches.empty()) {
      auto* row =
          new SettingRow(result.ok ? QString("Nothing on Steam matches")
                                   : "Could not search: " + error_help::Describe(result.error),
                         QString(), page);
      SetTextStyle(row->Label(), text::kSubtle);
      page->AddRow(row);
      return;
    }
    const auto shown = result.matches | std::views::take(kMaxTitles);
    QueueCovers(page, {shown.begin(), shown.end()});
    for (const TitleMatch& match : shown) {
      const QString name = QString::fromStdString(match.name);
      const QString ref = QString::number(match.appid);
      auto* row = new SettingRow(QString(), QString(), page);
      auto* cover = new QLabel(row);
      cover->setFixedSize(kResultCover);
      const auto draw = [this, cover, ref, name] {
        cover->setPixmap(
            artwork_->TitleCover("steam", ref, name, kResultCover, devicePixelRatioF()));
      };
      draw();
      connect(artwork_, &ArtworkStore::CoverChanged, cover, [draw, ref](const QString& id) {
        if (id == "steam-" + ref) draw();
      });
      row->SetLeading(cover);
      AddTwoLines(row, new ElidedLabel(name, row))->setText("Steam " + ref);
      if (match.appid == appid_) row->AddControl(new Badge("Current", Tone::Accent, row));
      auto* pick = new QPushButton("Use this", row);
      connect(pick, &QPushButton::clicked, page,
              [use, appid = match.appid, name] { use(appid, name); });
      row->AddControl(pick);
      page->AddRow(row);
    }
  };
  const auto run = [this, search, show] {
    const QString text = search->text().trimmed();
    const int request = ++request_;
    if (text.isEmpty()) return;
    api::SearchTitlesAsync(search, text.toStdString(),
                           [this, request, show](TitleMatchesResult result) {
                             if (request == request_) show(result);
                           });
  };
  auto* delay = new QTimer(page);
  delay->setSingleShot(true);
  delay->setInterval(350);
  connect(delay, &QTimer::timeout, page, run);
  connect(search, &QLineEdit::textChanged, delay, qOverload<>(&QTimer::start));
  connect(search, &QLineEdit::returnPressed, page, run);

  QHBoxLayout* buttons = page->ButtonRow();
  auto* none = new Button("None of these, use the name only", ButtonKind::Link, page);
  connect(none, &QPushButton::clicked, page, [use] { use(0, QString()); });
  buttons->insertWidget(0, none);

  page_->hide();
  page->show();
  search->setFocus();
  search->selectAll();
  run();
}

void GameSetupCard::CloseTitles() {
  if (titles_ == nullptr) return;
  layout_->removeWidget(titles_);
  titles_->hide();
  titles_->deleteLater();
  titles_ = nullptr;
  page_->show();
}

std::vector<GameSetupCard::Program> GameSetupCard::GamePrograms() const {
  std::vector<GameDetail::Candidate> sorted = game_.candidates;
  std::ranges::stable_sort(sorted, std::greater{}, &GameDetail::Candidate::score);
  std::vector<Program> programs;
  const QString folder = QString::fromStdString(game_.install_path) + "/";
  const auto add = [&](const std::string& path, bool installer) {
    const QString rel = QString::fromStdString(path);
    const QFileInfo file(QFileInfo(rel).isAbsolute() ? rel : folder + rel);
    const Program program{rel, file.exists() ? file.size() : -1, installer};
    // What mirad chose first.
    path == game_.exe_path ? programs.insert(programs.begin(), program)
                           : programs.insert(programs.end(), program);
  };
  for (const GameDetail::Candidate& candidate : sorted)
    add(candidate.rel_path, candidate.is_installer);
  if (!game_.exe_path.empty() &&
      std::ranges::none_of(sorted, [&](const GameDetail::Candidate& candidate) {
        return candidate.rel_path == game_.exe_path;
      }))
    add(game_.exe_path, game_.setup == "install");
  return programs;
}

void GameSetupCard::SetPrograms(const std::vector<Program>& programs) {
  programs_ = programs;
  const auto facts = [](const Program& program) {
    return program.bytes < 0 ? QString() : SizeText(program.bytes);
  };
  if (program_ != nullptr) {
    program_->clear();
    for (const Program& program : programs_) {
      const QString size = facts(program);
      const QString path =
          QFileInfo(program.path).isAbsolute() ? HomeRelative(program.path) : program.path;
      program_->addItem(size.isEmpty() ? path : path + kDot + size, program.path);
    }
    if (programs_.empty())
      program_->addItem(!IsArchive()               ? QString("None found")
                        : archive_.password_needed ? QString("Read from the archive once it opens")
                                                   : QString("Chosen once it's extracted"));
    program_->setEnabled(!programs_.empty());
  } else {
    for (QAbstractButton* button : program_list_->buttons()) delete button;
    for (std::size_t i = 0; i < programs_.size() && i < 6; ++i) {
      const Program& program = programs_[i];
      const QString size = facts(program);
      auto* radio = new QRadioButton(size.isEmpty() ? program.path : program.path + kDot + size);
      radio->setChecked(i == 0);
      program_list_->addButton(radio, static_cast<int>(i));
      program_list_layout_->addWidget(radio);
    }
    connect(program_list_, &QButtonGroup::idClicked, this, &GameSetupCard::Refresh,
            Qt::UniqueConnection);
  }
  Refresh();
}

void GameSetupCard::Browse() {
  const QString prefix = QString::fromStdString(game_.data_dir) + "/drive_c";
  const QString start = !game_.data_dir.empty() && game_.setup != "confirm" &&
                                QFileInfo::exists(prefix) && programs_.empty()
                            ? prefix
                            : QString::fromStdString(game_.install_path);
  const QString picked =
      QFileDialog::getOpenFileName(this, "The program that starts or installs it", start);
  if (picked.isEmpty()) return;
  const QString path = RelativeIfInside(picked, game_.install_path);
  std::vector<Program> programs = programs_;
  std::erase_if(programs, [&](const Program& program) { return program.path == path; });
  programs.insert(programs.begin(), {path, QFileInfo(picked).size(), false});
  picked_kind_ = false;
  SetPrograms(programs);
}

void GameSetupCard::LookAgain() {
  SetBusy(true);
  error_->hide();
  api::DetectGameAsync(this, game_.id, [this](PatchGameResult result) {
    if (!result.ok) {
      SetBusy(false);
      return Fail("Could not look again: ", result.error);
    }
    api::GetGameAsync(this, game_.id, [this](GameDetailResult detail) {
      SetBusy(false);
      if (!detail.ok) return Fail("Could not look again: ", detail.error);
      game_.candidates = detail.game.candidates;
      game_.exe_path = detail.game.exe_path;
      SetPrograms(GamePrograms());
      if (programs_.empty()) {
        error_->setText("Still no program in its folder.");
        error_->show();
      } else if (problem_ != nullptr) {
        problem_->parentWidget()->hide();
      }
    });
  });
}

void GameSetupCard::LoadInstaller() {
  if (IsArchive()) return;
  const QString program = ChosenProgram();
  if (program.isEmpty()) return;
  const QString path = QFileInfo(program).isAbsolute()
                           ? program
                           : QString::fromStdString(game_.install_path) + "/" + program;
  const int request = ++installer_request_;
  api::GetInstallerInfoAsync(this, game_.id, path.toStdString(),
                             [this, request](InstallerInfoResult info) {
                               if (request != installer_request_) return;
                               installer_ = info;
                               Refresh();
                             });
}

void GameSetupCard::ShowArchive(const ArchiveContentsResult& contents) {
  archive_ = contents;
  const FoundArchive& archive = archive_.archive;
  QStringList facts{SizeText(archive.bytes)};
  if (archive.volumes > 1) facts << Counted(archive.volumes, "part");
  if (archive_.password_needed) {
    facts << "none of your saved passwords open it";
  } else if (archive_.unpacked_bytes > 0) {
    facts << SizeText(archive_.unpacked_bytes) + " unpacked";
  }
  archive_facts_->setText(facts.join(kDot));
  password_row_->setVisible(archive_.password_needed);
  std::vector<Program> programs;
  for (const ArchiveContentsResult::Program& program : archive_.programs)
    programs.push_back({QString::fromStdString(program.path),
                        program.bytes > 0 ? program.bytes : -1, program.installer});
  SetPrograms(programs);
  IdentifyByName();
  if (archive_.password_needed) password_edit_->setFocus();
  // Extracting it can't end well.
  if (archive_.free_bytes >= 0 && archive_.unpacked_bytes > archive_.free_bytes) {
    error_->setText(QString("It unpacks to %1, but only %2 is free there.")
                        .arg(SizeText(archive_.unpacked_bytes), SizeText(archive_.free_bytes)));
    error_->show();
  }
}

void GameSetupCard::OpenWithPassword() {
  if (busy_ || password_edit_->text().isEmpty()) return;
  SetBusy(true);
  error_->hide();
  const std::string password = password_edit_->text().toStdString();
  api::InspectArchiveAsync(this, archive_.archive.path, password,
                           [this, password](ArchiveContentsResult contents) {
                             SetBusy(false);
                             if (!contents.ok)
                               return Fail("Could not read the archive: ", contents.error);
                             if (contents.password_needed) {
                               error_->setText("Wrong password.");
                               error_->show();
                               password_edit_->setFocus();
                               password_edit_->selectAll();
                               return;
                             }
                             password_ = password;
                             ShowArchive(contents);
                           });
}

QString GameSetupCard::ChosenProgram() const {
  if (programs_.empty()) return {};
  const int at = program_ != nullptr ? program_->currentIndex() : program_list_->checkedId();
  return at >= 0 && at < static_cast<int>(programs_.size())
             ? programs_[static_cast<std::size_t>(at)].path
             : QString();
}

bool GameSetupCard::Installs() const {
  return does_->Value() == "installs";
}

void GameSetupCard::Refresh() {
  if (main_ == nullptr) return;  // still being built
  const QString name = name_->text().trimmed();
  heading_->setText(name.isEmpty() ? QString("Set it up") : "Set up " + name);
  generic_->setVisible(IsGenericName(name));
  const QString chosen = ChosenProgram();
  const auto program = std::ranges::find(programs_, chosen, &Program::path);
  const bool guessed = !installed_ && program != programs_.end() && program->installer;
  if (!picked_kind_) does_->SetValue(guessed ? "installs" : "starts");
  does_hint_->setVisible(guessed && !picked_kind_);
  // What was just installed doesn't install it.
  does_->parentWidget()->setVisible(!installed_);

  const bool installs = Installs() && !IsArchive();
  QStringList facts;
  if (installs && installer_.ok) {
    facts << SizeText(installer_.size_bytes);
    if (installer_.estimated_install_bytes > 0)
      facts << "about " + SizeText(installer_.estimated_install_bytes) + " installed";
    if (const QString format = InstallerFormatName(installer_.format); !format.isEmpty())
      facts << format;
    if (!installer_.silent) facts << "opens its own window";
  }
  installer_line_->setText(facts.join(kDot));
  installer_row_->setVisible(!facts.isEmpty());
  if (installs && installer_.ok &&
      target_->count() != static_cast<int>(installer_.targets.size())) {
    target_->clear();
    for (const InstallerInfoResult::Target& target : installer_.targets)
      target_->addItem(HomeRelative(QString::fromStdString(target.path)) + kDot +
                           SizeText(target.free_bytes) + " free",
                       QString::fromStdString(target.path));
  }
  target_row_->setVisible(installs && target_->count() > 0);

  const bool opened = !IsArchive() || !archive_.password_needed;
  for (QWidget* field : std::initializer_list<QWidget*>{title_row_, name_, does_, kind_})
    field->setEnabled(opened);
  const bool ready = !busy_ && opened && !name.isEmpty() && (IsArchive() || !chosen.isEmpty());
  shown_->setVisible(installs && installer_.ok && installer_.silent);
  shown_->setEnabled(ready);
  main_->setText(IsArchive()                          ? "Extract and add"
                 : !installs                          ? "Confirm"
                 : installer_.ok && installer_.silent ? "Install quietly"
                                                      : "Show the installer");
  main_->setEnabled(ready);
  if (foot_ != nullptr) foot_->setVisible(!installs);
  if (other_ != nullptr) other_->setEnabled(!busy_);
}

SetupChoices GameSetupCard::Choices() const {
  SetupChoices choices;
  choices.name = name_->text().trimmed().toStdString();
  choices.app = kind_->Value() == "app";
  if (const QString program = ChosenProgram(); !program.isEmpty()) {
    choices.exe_path = program.toStdString();
    choices.installer = Installs();
  }
  choices.steam_appid = picked_appid_;
  // What the card showed as its title is what it's set up as.
  if (!picked_appid_ && IsArchive() && appid_ > 0) choices.steam_appid = appid_;
  return choices;
}

void GameSetupCard::Confirm(std::optional<bool> interactive) {
  SetBusy(true);
  error_->hide();
  api::ConfirmGameAsync(this, game_.id, Choices(), [this, interactive](GameDetailResult result) {
    if (!result.ok) {
      SetBusy(false);
      return Fail("Could not set it up: ", result.error);
    }
    if (!interactive) return emit CloseRequested();
    const std::string target =
        target_->count() > 0 ? target_->currentData().toString().toStdString() : std::string();
    api::InstallGameAsync(this, game_.id, *interactive, std::string(), target,
                          [this](GameActionResult started) {
                            if (started.ok) return emit CloseRequested();
                            SetBusy(false);
                            Fail("Could not start the install: ", started.error);
                          });
  });
}

void GameSetupCard::Extract() {
  SetBusy(true);
  error_->hide();
  api::ExtractArchiveAsync(this, archive_.archive.path, password_, delete_archive_->isChecked(),
                           Choices(), [this](GameActionResult result) {
                             // Its progress shows in Activity.
                             if (result.ok) return emit CloseRequested();
                             SetBusy(false);
                             Fail("Could not extract it: ", result.error);
                           });
}

void GameSetupCard::Fail(const QString& what, const ApiError& why) {
  error_->setText(what + error_help::Describe(why));
  error_->show();
}

void GameSetupCard::SetBusy(bool busy) {
  busy_ = busy;
  Refresh();
}

}  // namespace mira_gui
