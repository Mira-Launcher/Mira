#include "SetupPages.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLibrary>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStorageInfo>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <json.hpp>

#include "../bigscreen/Session.h"
#include "../client/EventHub.h"
#include "../client/api/Config.h"
#include "../client/api/Library.h"
#include "../client/api/Runners.h"
#include "../client/api/Stores.h"
#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"
#include "../settings/SettingsCard.h"
#include "../sources/SourceText.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ProgressRail.h"
#include "OfficePanel.h"
#include "SetupLook.h"
#include "SetupWork.h"
#include "SignInGuide.h"
#include "SignInPanel.h"

namespace mira_gui {
namespace {

QString HomeRelative(const QString& path) {
  const QString home = QDir::homePath();
  return path == home || path.startsWith(home + "/") ? "~" + path.mid(home.size()) : path;
}

QString Json(const QStringList& items) {
  nlohmann::json list = nlohmann::json::array();
  for (const QString& item : items) list.push_back(item.toStdString());
  return QString::fromStdString(list.dump());
}

// "a, b and c".
QString Listed(const QStringList& items) {
  if (items.size() < 2) return items.join("");
  return items.mid(0, items.size() - 1).join(", ") + " and " + items.back();
}

void Repolish(QWidget* widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
}

QLabel* Bold(QWidget* parent, const QString& text) {
  auto* label = new QLabel(text, parent);
  label->setObjectName("setup_strong");
  label->setWordWrap(true);
  return label;
}

QLabel* GlyphLabel(QWidget* parent, icons::Glyph glyph, int size, const QColor& color) {
  auto* label = new QLabel(parent);
  label->setPixmap(icons::For(glyph, color).pixmap(QSize(size, size), parent->devicePixelRatioF()));
  label->setFixedSize(size, size);
  return label;
}

// The tick in a picked tile's corner, following the tile.
QLabel* Tick(QAbstractButton* tile) {
  auto* tick = new QLabel(tile);
  tick->setObjectName("setup_tick");
  tick->setFixedSize(17, 17);
  tick->setAlignment(Qt::AlignCenter);
  tick->setAttribute(Qt::WA_TransparentForMouseEvents);
  const auto show = [tick](bool on) {
    tick->setText(on ? QString::fromUtf8("\xe2\x9c\x93") : QString());
    tick->setProperty("on", on);
    Repolish(tick);
  };
  QObject::connect(tile, &QAbstractButton::toggled, tick, show);
  show(tile->isChecked());
  return tick;
}

// A button sized by the layout inside it. QPushButton's own size hint only counts its text, so
// wrapped labels inside got squeezed whenever the fonts were bigger than when it was made.
class LaidOutButton : public QPushButton {
 public:
  using QPushButton::QPushButton;
  QSize sizeHint() const override {
    return layout() != nullptr ? layout()->sizeHint() : QPushButton::sizeHint();
  }
  QSize minimumSizeHint() const override {
    return layout() != nullptr ? layout()->minimumSize() : QPushButton::minimumSizeHint();
  }
  bool hasHeightForWidth() const override {
    return layout() != nullptr && layout()->hasHeightForWidth();
  }
  int heightForWidth(int width) const override {
    return layout() != nullptr ? layout()->totalHeightForWidth(width)
                               : QPushButton::heightForWidth(width);
  }
};

// A picked-or-not card: a choice_card button whose content is laid out inside it.
QPushButton* Tile(QWidget* parent) {
  auto* tile = new LaidOutButton(parent);
  tile->setObjectName("choice_card");
  tile->setCheckable(true);
  tile->setCursor(Qt::PointingHandCursor);
  return tile;
}

// Every child passes clicks to the tile.
void FinishTile(QPushButton* tile) {
  for (QWidget* child : tile->findChildren<QWidget*>()) {
    child->setAttribute(Qt::WA_TransparentForMouseEvents);
  }
  // Tiles side by side grow to the tallest one's height.
  QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  policy.setHeightForWidth(tile->hasHeightForWidth());
  tile->setSizePolicy(policy);
}

QFrame* Note(QWidget* parent, icons::Glyph glyph, QLabel* text) {
  auto* note = new QFrame(parent);
  note->setObjectName("setup_note");
  auto* line = new QHBoxLayout(note);
  line->setContentsMargins(12, 10, 12, 10);
  line->setSpacing(10);
  line->addWidget(GlyphLabel(note, glyph, 18, theme::Current().info), 0, Qt::AlignTop);
  text->setParent(note);
  text->setWordWrap(true);
  text->setTextFormat(Qt::RichText);
  line->addWidget(text, /*stretch=*/1);
  return note;
}

void Clear(QLayout* layout) {
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (QWidget* widget = item->widget()) {
      widget->hide();
      widget->deleteLater();
    } else if (QLayout* inner = item->layout()) {
      Clear(inner);
    }
    delete item;
  }
}

QString StoreLine(const QString& id) {
  static const QHash<QString, QString> kLines = {
      {"steam", "Also lists the games you own but haven't installed"},
      {"epic", "Your library, including the weekly free games you claimed"},
      {"gog", "DRM-free games, installed and updated by Mira"},
      {"amazon", "Includes the games you got with Prime Gaming"},
      {"itch", "Indie games and bundles you've bought or claimed"},
      {"humble", "Downloads the DRM-free games from your bundles"},
  };
  return kLines.value(id);
}

QString StoreName(const QString& id) {
  const SourceInfo* source = FindSourceInfo(id);
  return source != nullptr ? source->name : id;
}

QLabel* Chip(QWidget* parent, const QString& text, const char* state) {
  auto* chip = new QLabel(text, parent);
  chip->setObjectName("setup_chip");
  chip->setProperty("state", state);
  return chip;
}

}  // namespace

// --- Welcome ---------------------------------------------------------------------------------

namespace {

// The three example games' heroes side by side, fading into the window, with Mira's name.
class HeroStrip : public QWidget {
 public:
  HeroStrip(DemoArt* art, QWidget* parent) : QWidget(parent), art_(art) {
    setFixedHeight(150);
    connect(art, &DemoArt::Loaded, this, qOverload<>(&QWidget::update));
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    const theme::Tokens& tokens = theme::Current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto samples = theme::SampleArt(tokens);
    const int count = static_cast<int>(DemoArt::Games().size());
    for (int i = 0; i < count; ++i) {
      const QRect cell(i * width() / count, 0, (i + 1) * width() / count - i * width() / count,
                       height());
      const QPixmap hero = art_->Hero(i);
      if (hero.isNull()) {
        painter.fillRect(cell, samples[i].darker(170));
        continue;
      }
      const QPixmap scaled =
          hero.scaled(cell.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
      painter.drawPixmap(cell, scaled,
                         QRect((scaled.width() - cell.width()) / 2,
                               (scaled.height() - cell.height()) / 2, cell.width(), cell.height()));
    }
    QLinearGradient fade(0, 0, 0, height());
    QColor clear = tokens.window;
    clear.setAlpha(0);
    fade.setColorAt(0.2, clear);
    fade.setColorAt(0.96, tokens.window);
    fade.setColorAt(1, tokens.window);
    painter.fillRect(rect(), fade);

    QFont word = font();
    word.setPixelSize(32);
    word.setWeight(QFont::Bold);
    painter.setFont(word);
    painter.setPen(tokens.text);
    const int base = height() - 14 - 20;
    painter.drawText(QPoint(36, base), "Mira");
    QFont line = font();
    line.setPixelSize(13);
    painter.setFont(line);
    painter.setPen(tokens.text_muted);
    painter.drawText(QPoint(37, height() - 14), "Your games and Windows apps, one launcher");
  }

 private:
  DemoArt* art_;
};

}  // namespace

WelcomePage::WelcomePage(const SetupContext& context) : context_(context) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  outer->addWidget(new HeroStrip(context.art, this));
  auto* layout = new QVBoxLayout();
  layout->setContentsMargins(36, 14, 36, 22);
  layout->setSpacing(12);
  outer->addLayout(layout);

  auto* features = new QGridLayout();
  features->setSpacing(8);
  const struct {
    icons::Glyph glyph;
    const char* title;
    const char* text;
  } kFeatures[] = {
      {icons::Glyph::Layers, "Every store, one library",
       "Steam, Epic, GOG, Amazon, itch.io, Humble, Lutris and the games you install yourself."},
      {icons::Glyph::Wrench, "Windows games just run",
       "Mira picks and downloads Proton or Wine, and keeps each game in its own prefix."},
      {icons::Glyph::Grid, "Windows applications too",
       "Microsoft 365 installs like a game: Word, Excel and PowerPoint in your library."},
      {icons::Glyph::Monitor, "Big screen for the couch",
       "A full-screen mode built for a controller, with trailers and quick settings."},
  };
  for (int i = 0; i < 4; ++i) {
    auto* card = new QFrame(this);
    card->setObjectName("setup_feature");
    auto* line = new QHBoxLayout(card);
    line->setContentsMargins(12, 10, 12, 10);
    line->setSpacing(10);
    line->addWidget(GlyphLabel(card, kFeatures[i].glyph, 20, theme::Current().accent), 0,
                    Qt::AlignTop);
    auto* text = new QVBoxLayout();
    text->setSpacing(1);
    text->addWidget(Bold(card, kFeatures[i].title));
    text->addWidget(MakeLabel(card, kFeatures[i].text, "muted"));
    line->addLayout(text, /*stretch=*/1);
    features->addWidget(card, i / 2, i % 2);
  }
  layout->addLayout(features);

  QFrame* folder = SetupCard(this);
  auto* card = new QVBoxLayout(folder);
  card->setContentsMargins(16, 14, 16, 14);
  card->setSpacing(8);
  auto* head = new QHBoxLayout();
  head->setSpacing(8);
  head->addWidget(GlyphLabel(folder, icons::Glyph::Folder, 18, theme::Current().accent));
  head->addWidget(Bold(folder, "Where Mira keeps your games and apps"), /*stretch=*/1);
  card->addLayout(head);
  auto* path_row = new QHBoxLayout();
  path_row->setSpacing(8);
  path_ = new QLabel(folder);
  path_->setObjectName("setup_path");
  path_row->addWidget(path_, /*stretch=*/1);
  auto* change = new QPushButton("Change…", folder);
  connect(change, &QPushButton::clicked, this, [this] {
    const QString picked = QFileDialog::getExistingDirectory(
        window(), "Where Mira keeps your games and apps", folder_);
    if (!picked.isEmpty()) ChooseFolder(picked);
  });
  path_row->addWidget(change);
  card->addLayout(path_row);
  auto* space = new QHBoxLayout();
  space->setSpacing(12);
  used_ = new ProgressRail(folder);
  space->addWidget(used_, /*stretch=*/1);
  free_ = MakeLabel(folder, QString(), "muted", false);
  space->addWidget(free_);
  card->addLayout(space);
  drive_warning_ = MakeLabel(
      folder, "Windows games can fail to start or lose saves on NTFS and exFAT drives.", "error");
  QSizePolicy keep = drive_warning_->sizePolicy();
  keep.setRetainSizeWhenHidden(true);
  drive_warning_->setSizePolicy(keep);
  drive_warning_->hide();
  card->addWidget(drive_warning_);
  layout->addWidget(folder);

  // The packages Next installs, shown only when some are missing.
  auto* note_body = new QWidget(this);
  auto* note_column = new QVBoxLayout(note_body);
  note_column->setContentsMargins(0, 0, 0, 0);
  note_column->setSpacing(4);
  note_text_ = MakeLabel(note_body, QString());
  note_text_->setTextFormat(Qt::RichText);
  note_column->addWidget(note_text_);
  choose_ = new QPushButton("Choose packages", note_body);
  choose_->setObjectName("text_button");
  connect(choose_, &QPushButton::clicked, this,
          [this] { package_list_->setVisible(!package_list_->isVisible()); });
  auto* choose_row = new QHBoxLayout();
  choose_row->addWidget(choose_);
  choose_row->addStretch(1);
  note_column->addLayout(choose_row);
  package_list_ = new QWidget(note_body);
  package_list_->setLayout(new QVBoxLayout());
  package_list_->layout()->setContentsMargins(0, 0, 0, 0);
  package_list_->hide();
  note_column->addWidget(package_list_);
  auto* holder = new QLabel();
  note_ = Note(this, icons::Glyph::Info, holder);
  // The note's own text label is replaced by the column above.
  note_->layout()->replaceWidget(holder, note_body);
  holder->deleteLater();
  note_->hide();
  layout->addWidget(note_);

  layout->addWidget(SetupLaterLine(
      this,
      "Every page can be skipped, and so can all of setup: <b>Skip setup</b> opens Mira as it "
      "is, and you can set it all up later if you wish."));
  layout->addStretch(1);

  folder_ = QDir::homePath() + "/Mira";
  ShowFolder();
  api::GetConfigAsync(this, [this](ConfigResult result) {
    if (!result.ok) return;
    const nlohmann::json roots =
        nlohmann::json::parse(result.values["library_roots"], nullptr, false);
    if (roots.is_array() && !roots.empty() && roots.front().is_string()) {
      QString first = QString::fromStdString(roots.front().get<std::string>());
      if (first.startsWith("~")) first = QDir::homePath() + first.mid(1);
      folder_ = QFileInfo(first).path();
    }
    ShowFolder();
  });
  api::GetSystemPackagesAsync(this, {}, [this](SystemPackagesResult result) {
    packages_ = std::move(result);
    ShowPackages();
  });
  api::ListRunnersAsync(this, [this](RunnersResult result) {
    if (!result.ok) return;
    runners_known_ = true;
    windows_ready_ = std::ranges::any_of(
        result.runners, [](const RunnerInfo& r) { return r.kind == "proton" || r.kind == "wine"; });
  });
}

void WelcomePage::ShowFolder() {
  path_->setText(HomeRelative(folder_));
  // The drive it's on, through the nearest folder that exists yet.
  QString existing = folder_;
  while (!QFileInfo::exists(existing) && existing != QFileInfo(existing).path()) {
    existing = QFileInfo(existing).path();
  }
  const QStorageInfo drive(existing);
  const qint64 total = drive.bytesTotal();
  used_->SetProgress(total > 0 ? 1.0 - static_cast<double>(drive.bytesAvailable()) / total : 0);
  free_->setText(
      QString("%1 free of %2").arg(SizeText(drive.bytesAvailable()), SizeText(total)));
  static const QSet<QByteArray> kRisky = {"ntfs", "ntfs3", "fuseblk", "exfat", "vfat", "msdos"};
  drive_warning_->setVisible(kRisky.contains(drive.fileSystemType()));
}

void WelcomePage::ChooseFolder(const QString& folder) {
  // A library folder that already holds games stays watched; only empty ones move.
  QStringList roots = {folder + "/Games", folder + "/Applications"};
  for (const QString& old : {folder_ + "/Games", folder_ + "/Applications"}) {
    const QDir dir(old);
    if (dir.exists() && !dir.isEmpty() && !roots.contains(old)) roots << old;
  }
  const std::vector<ConfigEdit> edits = {
      {"library_roots", "an array of strings", Json(roots).toStdString()},
      {"prefix_root", "a string", (folder + "/prefixes").toStdString()},
      {"epic.install_root", "a string", (folder + "/Epic Games").toStdString()},
      {"gog.install_root", "a string", (folder + "/GOG").toStdString()},
      {"itch.install_root", "a string", (folder + "/itch.io").toStdString()},
      {"amazon.install_root", "a string", (folder + "/Amazon Games").toStdString()},
      {"humble.download_root", "a string", (folder + "/Humble Bundle").toStdString()},
  };
  const QString before = folder_;
  folder_ = folder;
  ShowFolder();
  api::PatchConfigAsync(this, edits, [this, before](PatchConfigResult result) {
    if (result.ok) return;
    folder_ = before;
    ShowFolder();
    ShowError(drive_warning_, "Could not change the folder.", result.error);
  });
}

void WelcomePage::ShowPackages() {
  note_->setVisible(packages_.ok && !packages_.missing.empty());
  if (!note_->isVisibleTo(this)) return;
  const bool can_install = packages_.install.size() >= packages_.missing.size() &&
                           !packages_.install.empty();
  QStringList chosen;
  for (const std::string& package : packages_.missing) {
    if (!unticked_.contains(package)) chosen << QString::fromStdString(package);
  }
  const QString count =
      chosen.size() == 1 ? QString("1 system package") : QString("%1 system packages").arg(chosen.size());
  if (!can_install) {
    QStringList all;
    for (const std::string& package : packages_.missing) all << QString::fromStdString(package);
    note_text_->setText(QString("Mira uses %1 that aren't installed: %2. Install them with your "
                                "system's package manager.")
                            .arg(all.size() == 1 ? QString("a package") : QString("packages"),
                                 Listed(all).toHtmlEscaped()));
  } else if (chosen.isEmpty()) {
    note_text_->setText("No system packages are installed. Mira works without them, with less.");
  } else {
    note_text_->setText(
        QString("<b>Next asks for your password once.</b> Mira installs %1 it uses: %2.")
            .arg(count, Listed(chosen).toHtmlEscaped()));
  }
  choose_->setVisible(can_install);
  if (package_list_->layout()->count() > 0 || !can_install) return;
  for (std::size_t i = 0; i < packages_.missing.size(); ++i) {
    const std::string package = packages_.missing[i];
    const QString purpose =
        i < packages_.purposes.size() ? QString::fromStdString(packages_.purposes[i]) : QString();
    auto* tick = new QCheckBox(package_list_);
    tick->setText(QString::fromStdString(package) + (purpose.isEmpty() ? "" : "  ·  " + purpose));
    tick->setChecked(true);
    connect(tick, &QCheckBox::toggled, this, [this, package](bool on) {
      if (on) {
        unticked_.erase(package);
      } else {
        unticked_.insert(package);
      }
      ShowPackages();
    });
    package_list_->layout()->addWidget(tick);
  }
}

void WelcomePage::Leave() {
  if (packages_.ok && !packages_.install.empty() &&
      packages_.install.size() >= packages_.missing.size()) {
    std::vector<std::string> chosen;
    for (const std::string& package : packages_.missing) {
      if (!unticked_.contains(package)) chosen.push_back(package);
    }
    // mirad's command ends with every missing package; only the ticked ones go.
    std::vector<std::string> argv(packages_.install.begin(),
                                  packages_.install.end() -
                                      static_cast<std::ptrdiff_t>(packages_.missing.size()));
    argv.insert(argv.end(), chosen.begin(), chosen.end());
    if (!chosen.empty()) context_.work->InstallPackages(window(), argv, chosen, packages_.restart);
  }
  if (runners_known_ && !windows_ready_) context_.work->DownloadProton();
}

// --- What for --------------------------------------------------------------------------------

UsePage::UsePage(const SetupContext& context) : context_(context) {
  QVBoxLayout* layout = SetupPageLayout(
      this, icons::Glyph::Search, "Getting started", "What do you want to configure right now?",
      "Pick one or both. The next pages will let you configure your sources for "
      "each pick.");
  auto* row = new QHBoxLayout();
  row->setSpacing(12);
  const auto tile = [this, &context](bool games) {
    QPushButton* button = Tile(this);
    auto* inside = new QVBoxLayout(button);
    inside->setContentsMargins(0, 0, 0, 14);
    inside->setSpacing(0);
    auto* art = new QFrame(button);
    art->setObjectName("setup_art");
    art->setFixedHeight(118);
    auto* art_layout = new QGridLayout(art);
    art_layout->setContentsMargins(0, 0, 10, 0);
    CoverFan* fan =
        games ? new CoverFan(context, [&context] { return PreviewGames(context, 3); },
                             QSize(60, 90), 40, 8, art)
              : new CoverFan(
                    context,
                    [] {
                      std::vector<Picture> docs;
                      for (const auto& [ref, letter] :
                           {std::pair{"word", "W"}, {"excel", "X"}, {"powerpoint", "P"}}) {
                        docs.push_back({.color = OfficeAppColor(ref), .letter = letter});
                      }
                      return docs;
                    },
                    QSize(52, 66), 46, 8, art);
    art_layout->addWidget(fan, 0, 0, Qt::AlignCenter);
    art_layout->addWidget(Tick(button), 0, 0, Qt::AlignRight | Qt::AlignTop);
    art_layout->setContentsMargins(0, 10, 10, 0);
    inside->addWidget(art);
    auto* text = new QVBoxLayout();
    text->setContentsMargins(14, 12, 14, 0);
    text->setSpacing(3);
    auto* name = Bold(button, games ? "Games" : "Windows applications");
    name->setObjectName("setup_tile_title");
    text->addWidget(name);
    text->addWidget(MakeLabel(button,
                              games ? "From Steam, other stores, or ones you install yourself."
                                    : "Programs that only exist for Windows, set up to run here.",
                              "muted"));
    inside->addLayout(text);
    inside->addStretch(1);
    FinishTile(button);
    return button;
  };
  games_ = tile(true);
  apps_ = tile(false);
  connect(games_, &QAbstractButton::toggled, this, [this](bool on) {
    context_.choices->games = on;
    emit PicksChanged();
  });
  connect(apps_, &QAbstractButton::toggled, this, [this](bool on) {
    context_.choices->apps = on;
    emit PicksChanged();
  });
  row->addWidget(games_, /*stretch=*/1);
  row->addWidget(apps_, /*stretch=*/1);
  layout->addLayout(row);
  layout->addWidget(
      SetupLaterLine(this, "These can be configured at any time later in the sidebar."));
  layout->addStretch(1);
}

void UsePage::Enter() {
  games_->setChecked(context_.choices->games);
  apps_->setChecked(context_.choices->apps);
}

// --- Found on this computer -------------------------------------------------------------------

FoundPage::FoundPage(const SetupContext& context) : context_(context) {
  QVBoxLayout* layout = SetupPageLayout(this, icons::Glyph::Search, "Games",
                                        "Found on this computer",
                                        "These come into Mira as they are. Nothing is moved or changed.");
  body_ = new QVBoxLayout();
  body_->setSpacing(10);
  layout->addLayout(body_);
  layout->addStretch(1);
  connect(context.services.library, &GameLibraryModel::Changed, this, [this] {
    if (asked_) Show();
  });
  connect(EventHub::Instance(), &EventHub::RunnersChanged, this, [this] {
    api::ListRunnersAsync(this, [this](RunnersResult result) {
      if (!result.ok) return;
      runners_ = result.runners;
      runners_known_ = true;
      Show();
    });
  });
  connect(context.work, &SetupWork::Changed, this, [this] {
    if (asked_ && !std::ranges::any_of(runners_, [](const RunnerInfo& r) {
          return r.kind == "proton" || r.kind == "wine";
        }))
      Show();
  });
}

int FoundPage::Count(const char* source) const {
  return static_cast<int>(
      std::ranges::count_if(context_.services.library->Games(),
                            [source](const GameSummary& g) { return g.source == source; }));
}

void FoundPage::Enter() {
  if (asked_) return;
  asked_ = true;
  vulkan_ = QLibrary("vulkan", 1).load();
  api::GetSteamInstalledAsync(this, [this](SteamInstalledResult result) {
    steam_ = std::move(result);
    Show();
  });
  api::GetSteamAccountsAsync(this, [this](SteamAccountsResult result) {
    accounts_ = std::move(result);
    account_ = QString::fromStdString(accounts_.selected);
    Show();
  });
  api::GetLutrisAsync(this, [this](LutrisStatusResult result) {
    lutris_found_ = result.ok && result.found;
    Show();
  });
  api::ListRunnersAsync(this, [this](RunnersResult result) {
    if (!result.ok) return;
    runners_ = result.runners;
    runners_known_ = true;
    Show();
  });
  api::GetGameModeStatusAsync(this, [this](GameModeStatusResult result) {
    gamemode_ = result;
    Show();
  });
  Show();
}

void FoundPage::Show() {
  Clear(body_);
  const theme::Tokens& tokens = theme::Current();
  const int steam_games = Count("steam");
  const int lutris_games = Count("lutris");

  if (steam_.found) {
    QFrame* card = SetupCard(this);
    auto* column = new QVBoxLayout(card);
    column->setContentsMargins(16, 14, 16, 14);
    column->setSpacing(6);
    auto* head = new QHBoxLayout();
    head->setSpacing(14);
    const std::vector<SteamInstalledGame> games = steam_.games;
    ArtworkStore* artwork = context_.services.artwork;
    auto* fan = new CoverFan(
        context_,
        [games, artwork, card] {
          std::vector<Picture> pictures;
          for (std::size_t i = 0; i < games.size() && i < 4; ++i) {
            const QString name = QString::fromStdString(games[i].name);
            pictures.push_back({.cover = artwork->TitleCover("steam",
                                                             QString::fromStdString(games[i].appid),
                                                             name, QSize(88, 132),
                                                             card->devicePixelRatioF()),
                                .name = name});
          }
          return pictures;
        },
        QSize(44, 66), 22, 6, card);
    connect(artwork, &ArtworkStore::CoverChanged, fan, qOverload<>(&QWidget::update));
    fan->setFixedSize(118, 70);
    if (!games.empty()) {
      head->addWidget(fan);
    } else {
      fan->hide();
      head->addWidget(MakeSourceBadge(*FindSourceInfo("steam"), 30, card));
    }
    auto* text = new QVBoxLayout();
    text->setSpacing(1);
    text->addWidget(Bold(card, "Steam"));
    QString detail;
    if (steam_games > 0) {
      detail = steam_games == 1 ? QString("1 game in your library")
                                : QString("%1 games in your library").arg(steam_games);
    } else if (games.empty()) {
      detail = "No games installed yet. Games you install in Steam show up by themselves.";
    } else {
      QStringList names;
      for (std::size_t i = 0; i < games.size() && i < 3; ++i) {
        names << QString::fromStdString(games[i].name);
      }
      detail = games.size() == 1 ? QString("1 installed game, %1").arg(names.front())
               : games.size() <= 3
                   ? QString("%1 installed games: %2").arg(games.size()).arg(Listed(names))
                   : QString("%1 installed games, including %2").arg(games.size()).arg(Listed(names));
    }
    text->addWidget(MakeLabel(card, detail, "muted"));
    head->addLayout(text, /*stretch=*/1);
    if (steam_games == 0 && !games.empty()) {
      auto* add = new Switch(card);
      add->setChecked(steam_on_);
      add->setAccessibleName("Add Steam's installed games");
      connect(add, &QAbstractButton::toggled, this, [this](bool on) { steam_on_ = on; });
      head->addWidget(add);
    }
    column->addLayout(head);

    // Whose games: owned games and playtime are read for one account.
    if (accounts_.accounts.size() > 1) {
      column->addSpacing(6);
      column->addWidget(MakeLabel(
          card,
          QString("Steam has %1 accounts signed in. Which one is yours?").arg(accounts_.accounts.size()),
          "muted"));
      auto* group = new QButtonGroup(card);
      const auto palette = theme::SampleArt(tokens);
      for (std::size_t i = 0; i < accounts_.accounts.size(); ++i) {
        const SteamAccount& account = accounts_.accounts[i];
        const QString id = QString::fromStdString(account.steamid64);
        const QString name = QString::fromStdString(
            account.persona_name.empty() ? account.account_name : account.persona_name);
        auto* choice = new LaidOutButton(card);
        choice->setObjectName("setup_account");
        choice->setCheckable(true);
        choice->setChecked(id == account_);
        group->addButton(choice);
        auto* line = new QHBoxLayout(choice);
        line->setContentsMargins(10, 7, 10, 7);
        line->setSpacing(10);
        auto* radio = new QLabel(choice);
        radio->setObjectName("setup_radio");
        radio->setFixedSize(16, 16);
        radio->setProperty("on", id == account_);
        line->addWidget(radio);
        auto* avatar = new QLabel(name.left(1).toUpper(), choice);
        avatar->setObjectName("setup_avatar");
        avatar->setFixedSize(28, 28);
        avatar->setAlignment(Qt::AlignCenter);
        avatar->setStyleSheet(QString("background: %1;").arg(palette[i % palette.size()].name()));
        line->addWidget(avatar);
        auto* who = new QVBoxLayout();
        who->setSpacing(0);
        who->addWidget(new QLabel(name, choice));
        if (QString::fromStdString(account.account_name) != name) {
          who->addWidget(MakeLabel(choice, QString::fromStdString(account.account_name), "muted"));
        } else if (i == 0) {
          who->addWidget(MakeLabel(choice, "Signed in last", "muted"));
        }
        line->addLayout(who, /*stretch=*/1);
        FinishTile(choice);
        connect(choice, &QPushButton::clicked, this, [this, id] {
          account_ = id;
          QMetaObject::invokeMethod(this, &FoundPage::Show, Qt::QueuedConnection);
        });
        column->addWidget(choice);
      }
    }
    body_->addWidget(card);
  }

  if (lutris_found_) {
    QFrame* card = SetupCard(this);
    auto* line = new QHBoxLayout(card);
    line->setContentsMargins(16, 12, 16, 12);
    line->setSpacing(12);
    line->addWidget(MakeSourceBadge(*FindSourceInfo("lutris"), 30, card));
    auto* text = new QVBoxLayout();
    text->setSpacing(1);
    text->addWidget(Bold(card, "Lutris"));
    text->addWidget(MakeLabel(card,
                              lutris_games > 0 ? QString("%1 games added").arg(lutris_games)
                                               : QString("Its Wine and Linux games, as Lutris runs them"),
                              "muted"));
    line->addLayout(text, /*stretch=*/1);
    if (lutris_games == 0) {
      auto* add = new Switch(card);
      add->setChecked(lutris_on_);
      add->setAccessibleName("Add Lutris's games");
      connect(add, &QAbstractButton::toggled, this, [this](bool on) { lutris_on_ = on; });
      line->addWidget(add);
    }
    body_->addWidget(card);
  }

  if (!steam_.found && !lutris_found_) {
    QFrame* card = SetupCard(this);
    auto* line = new QHBoxLayout(card);
    line->setContentsMargins(16, 12, 16, 12);
    line->setSpacing(12);
    auto* mark = new QLabel("?", card);
    mark->setObjectName("setup_mark");
    mark->setFixedSize(36, 36);
    mark->setAlignment(Qt::AlignCenter);
    line->addWidget(mark);
    auto* text = new QVBoxLayout();
    text->setSpacing(1);
    text->addWidget(Bold(card, "No games yet"));
    text->addWidget(MakeLabel(card,
                              "Steam and Lutris aren't installed. That's fine: your stores come "
                              "next, and games you install yourself can be added any time.",
                              "muted"));
    line->addLayout(text, /*stretch=*/1);
    body_->addWidget(card);
  }

  if (runners_known_) {
    QFrame* card = SetupCard(this);
    auto* column = new QVBoxLayout(card);
    column->setContentsMargins(16, 12, 16, 14);
    column->setSpacing(8);
    QStringList builds;
    for (const RunnerInfo& runner : runners_) {
      if (runner.kind == "proton" || runner.kind == "wine") builds << QString::fromStdString(runner.label);
    }
    column->addWidget(
        Bold(card, builds.isEmpty() ? "Getting ready for Windows games" : "Ready for Windows games"));
    auto* chips = new QHBoxLayout();
    chips->setSpacing(6);
    const QString tick = QString::fromUtf8("\xe2\x9c\x93 ");
    if (builds.isEmpty()) {
      QString downloading;
      for (const QString& line : context_.work->Running()) {
        if (line.contains("Proton")) downloading = line;
      }
      chips->addWidget(Chip(card,
                            downloading.isEmpty() ? QString("No Proton yet. Runners has it, any time.")
                                                  : downloading,
                            downloading.isEmpty() ? "warning" : "plain"));
    }
    for (const QString& build : builds.mid(0, 3)) chips->addWidget(Chip(card, tick + build, "ok"));
    if (gamemode_.ok) {
      chips->addWidget(gamemode_.installed ? Chip(card, tick + "GameMode", "ok")
                                           : Chip(card, "No GameMode", "plain"));
    }
    chips->addWidget(vulkan_ ? Chip(card, tick + "Vulkan", "ok")
                             : Chip(card, "No Vulkan driver", "error"));
    chips->addStretch(1);
    column->addLayout(chips);
    if (!vulkan_) {
      column->addWidget(MakeLabel(
          card, "Games won't start without it. Install your graphics driver's Vulkan package.",
          "error"));
    }
    body_->addWidget(card);
  }
}

void FoundPage::Leave() {
  context_.choices->found.clear();
  if (steam_.found && steam_on_) context_.choices->found << "steam";
  if (lutris_found_ && lutris_on_) context_.choices->found << "lutris";
  const bool add_steam = steam_.found && steam_on_ && Count("steam") == 0 && !steam_.games.empty();
  if (!account_.isEmpty() && account_.toStdString() != accounts_.selected) {
    // Steam's playtime is read for the account picked, so it's saved before the games come in.
    api::PatchConfigAsync(this, {{"steam.steamid64", "a string", account_.toStdString()}},
                          [work = context_.work, add_steam](PatchConfigResult) {
                            if (add_steam) work->ImportSteam();
                          });
  } else if (add_steam) {
    context_.work->ImportSteam();
  }
  if (lutris_found_ && lutris_on_ && Count("lutris") == 0) context_.work->ImportLutris();
}

// --- Stores ----------------------------------------------------------------------------------

StoresPage::StoresPage(const SetupContext& context) : context_(context) {
  QVBoxLayout* layout = SetupPageLayout(
      this, icons::Glyph::Store, "Games", "Where do you buy games?",
      "Mira lists everything you own there and installs it for you. Each one you pick gets a short "
      "sign-in page next.");
  auto* grid = new QGridLayout();
  grid->setSpacing(10);
  for (int i = 0; i < setup::kStores.size(); ++i) {
    const QString id = setup::kStores[i];
    QPushButton* tile = Tile(this);
    auto* inside = new QHBoxLayout(tile);
    inside->setContentsMargins(12, 12, 10, 12);
    inside->setSpacing(12);
    inside->addWidget(MakeSourceBadge(*FindSourceInfo(id), 36, tile), 0, Qt::AlignTop);
    auto* text = new QVBoxLayout();
    text->setSpacing(1);
    text->addWidget(Bold(tile, StoreName(id)));
    text->addWidget(MakeLabel(tile, StoreLine(id), "muted"));
    text->addStretch(1);
    inside->addLayout(text, /*stretch=*/1);
    inside->addWidget(Tick(tile), 0, Qt::AlignTop);
    FinishTile(tile);
    connect(tile, &QAbstractButton::toggled, this, [this] {
      QStringList picked;
      for (const QString& store : setup::kStores) {
        if (tiles_[store]->isChecked()) picked << store;
      }
      context_.choices->stores = picked;
      Show();
      emit PicksChanged();
    });
    tiles_[id] = tile;
    grid->addWidget(tile, i / 2, i % 2);
  }
  grid->setColumnStretch(0, 1);
  grid->setColumnStretch(1, 1);
  layout->addLayout(grid);
  count_ = MakeLabel(this, QString(), "muted");
  layout->addWidget(count_);
  layout->addWidget(SetupLaterLine(
      this, "Skip this, or leave a store out: every store can be added later from the sidebar."));
  layout->addStretch(1);
}

void StoresPage::Enter() {
  // Back on a skipped page, its picks count again until it's skipped again.
  context_.choices->stores_skipped = false;
  for (const auto& [id, tile] : tiles_) {
    const QSignalBlocker quiet(tile);
    tile->setChecked(context_.choices->stores.contains(id));
    // The tick follows toggled, which the blocker held back.
    if (auto* tick = tile->findChild<QLabel*>("setup_tick")) {
      tick->setText(tile->isChecked() ? QString::fromUtf8("\xe2\x9c\x93") : QString());
      tick->setProperty("on", tile->isChecked());
      Repolish(tick);
    }
  }
  Show();
}

void StoresPage::Show() {
  const QStringList& stores = context_.choices->stores;
  const bool tools = std::ranges::any_of(stores, [](const QString& id) { return id != "steam"; });
  count_->setText(stores.isEmpty() ? QString("None picked, so no sign-ins follow.")
                  : tools ? QString("%1 picked · their helper tools start downloading when you continue.")
                                .arg(stores.size())
                          : QString("%1 picked").arg(stores.size()));
}

void StoresPage::Leave() {
  context_.work->SetUpTools(context_.choices->stores);
}

// --- Windows applications --------------------------------------------------------------------

AppsPage::AppsPage(const SetupContext& context) : context_(context) {
  QVBoxLayout* layout = SetupPageLayout(
      this, icons::Glyph::Grid, "Windows applications", "Which Windows applications do you use?",
      "Mira installs each one into its own Windows setup, separate from your games. Each one you "
      "pick gets its own page next.");
  QPushButton* tile = Tile(this);
  auto* inside = new QHBoxLayout(tile);
  inside->setContentsMargins(12, 12, 10, 12);
  inside->setSpacing(12);
  inside->addWidget(MakeSourceBadge(*FindSourceInfo("office"), 36, tile), 0, Qt::AlignTop);
  auto* text = new QVBoxLayout();
  text->setSpacing(1);
  text->addWidget(Bold(tile, "Microsoft 365"));
  text->addWidget(MakeLabel(tile,
                            "Word, Excel, PowerPoint, Outlook, OneNote, Access and Publisher. "
                            "Needs a Microsoft 365 subscription.",
                            "muted"));
  inside->addLayout(text, /*stretch=*/1);
  inside->addWidget(Tick(tile), 0, Qt::AlignTop);
  FinishTile(tile);
  office_ = tile;
  connect(tile, &QAbstractButton::toggled, this, [this](bool on) {
    context_.choices->office = on;
    line_->setText(on ? "Its Windows setup starts getting ready when you continue."
                      : "More applications are coming.");
    emit PicksChanged();
  });
  layout->addWidget(tile);
  line_ = MakeLabel(this, QString(), "muted");
  layout->addWidget(line_);

  QFrame* card = SetupCard(this);
  auto* rows = new QVBoxLayout(card);
  rows->setContentsMargins(14, 4, 14, 4);
  rows->setSpacing(0);
  in_all_ = new Switch(card);
  rows->addWidget(SetupRow(card, "Apps under All", "List apps under the All tab too, not only under Apps.",
                           in_all_));
  rows->addWidget(MakeDivider(card, Qt::Horizontal));
  big_screen_ = new Switch(card);
  rows->addWidget(SetupRow(card, "Apps in big screen",
                           "Show Word, Excel and the rest in big screen too.", big_screen_));
  layout->addWidget(card);
  layout->addWidget(SetupLaterLine(
      this,
      "Skip this and nothing gets installed. Applications can be added later from the sidebar."));
  layout->addStretch(1);
}

void AppsPage::Enter() {
  context_.choices->apps_skipped = false;
  office_->setChecked(context_.choices->office);
  line_->setText(context_.choices->office ? "Its Windows setup starts getting ready when you continue."
                                          : "More applications are coming.");
  in_all_->setChecked(context_.prefs->library_apps_in_all.value_or(true));
  big_screen_->setChecked(context_.prefs->big_screen_show_apps.value_or(false));
}

void AppsPage::Leave() {
  FrontendPrefs change;
  change.library_apps_in_all = in_all_->isChecked();
  change.big_screen_show_apps = big_screen_->isChecked();
  SaveSetupPrefs(context_, change);
  if (context_.choices->office) context_.work->PrepareOffice(window());
}

// --- A store's sign-in -----------------------------------------------------------------------

StorePage::StorePage(const SetupContext& context, const QString& store)
    : context_(context), store_(store) {
  const SignInGuide guide = GuideFor(store.toStdString());
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(36, 14, 36, 22);
  layout->setSpacing(12);
  auto* head = new QHBoxLayout();
  head->setSpacing(12);
  head->addWidget(MakeSourceBadge(*FindSourceInfo(store), 44, this));
  auto* text = new QVBoxLayout();
  text->setSpacing(2);
  eyebrow_ = new QLabel(this);
  eyebrow_->setProperty("role", "group_heading");
  text->addWidget(eyebrow_);
  auto* title = new QLabel(guide.title, this);
  title->setObjectName("setup_title");
  text->addWidget(title);
  head->addLayout(text, /*stretch=*/1);
  layout->addLayout(head);
  // The guide's own intro, when it has one, says what the store brings.
  if (guide.intro.isEmpty()) layout->addWidget(MakeLabel(this, StoreLine(store) + ".", "muted"));

  panel_ = new SignInPanel(store, this, /*header=*/false, /*stacked=*/true);
  layout->addWidget(panel_);
  done_ = MakeLabel(this, QString(), nullptr);
  done_->setObjectName("setup_signed_in");
  done_->hide();
  layout->addWidget(done_);
  connect(panel_, &SignInPanel::Connected, this, [this, guide](bool just_now) {
    if (store_ != "steam") {
      api::PatchSourceAsync(this, store_.toStdString(), true, std::nullopt,
                            [work = context_.work](PatchConfigResult) { emit work->SourcesChanged(); });
    }
    if (!just_now) return;
    done_->setText(QString::fromUtf8("\xe2\x9c\x93  ") + guide.done + ". Moving on…");
    done_->show();
    QTimer::singleShot(1500, this, [this] { emit Finished(); });
  });
  layout->addWidget(SetupLaterLine(
      this, QString("Not now? %1's page in the sidebar has this same sign-in, any time.")
                .arg(StoreName(store).toHtmlEscaped())));
  layout->addStretch(1);
}

void StorePage::Enter() {
  QStringList signs;
  for (const QString& key : setup::Flow(*context_.choices)) {
    if (key.startsWith("sign:")) signs << key;
  }
  eyebrow_->setText(QString("STORE %1 OF %2")
                        .arg(signs.indexOf("sign:" + store_) + 1)
                        .arg(signs.size()));
}

// --- Microsoft 365 ---------------------------------------------------------------------------

OfficeSetupPage::OfficeSetupPage(const SetupContext& context) : context_(context) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(36, 14, 36, 22);
  layout->setSpacing(12);
  auto* head = new QHBoxLayout();
  head->setSpacing(12);
  head->addWidget(MakeSourceBadge(*FindSourceInfo("office"), 44, this));
  auto* text = new QVBoxLayout();
  text->setSpacing(2);
  auto* eyebrow = new QLabel("APPLICATION 1 OF 1", this);
  eyebrow->setProperty("role", "group_heading");
  text->addWidget(eyebrow);
  auto* title = new QLabel("Set up Microsoft 365", this);
  title->setObjectName("setup_title");
  text->addWidget(title);
  head->addLayout(text, /*stretch=*/1);
  layout->addLayout(head);
  layout->addWidget(MakeLabel(
      this, "Office runs in its own Windows setup. Pick your plan and the apps you want. About 4 GB.",
      "muted"));

  QFrame* plan_card = SetupCard(this);
  auto* plan_rows = new QVBoxLayout(plan_card);
  plan_rows->setContentsMargins(14, 4, 14, 4);
  auto* segmented = new QWidget(plan_card);
  segmented->setObjectName("segmented");
  auto* buttons = new QHBoxLayout(segmented);
  buttons->setContentsMargins(3, 3, 3, 3);
  buttons->setSpacing(3);
  plans_ = new QButtonGroup(this);
  for (const OfficePlan& plan : OfficePlans()) {
    auto* button = new QPushButton(plan.short_name, segmented);
    button->setCheckable(true);
    button->setProperty("plan", plan.value);
    plans_->addButton(button);
    buttons->addWidget(button);
  }
  connect(plans_, &QButtonGroup::buttonClicked, this,
          [this](QAbstractButton* button) { plan_ = button->property("plan").toString(); });
  plan_rows->addWidget(SetupRow(plan_card, "Your plan", "Office installs the edition that matches it.",
                                segmented));
  layout->addWidget(plan_card);

  QFrame* apps_card = SetupCard(this);
  auto* apps = new QVBoxLayout(apps_card);
  apps->setContentsMargins(16, 12, 16, 14);
  apps->setSpacing(10);
  apps->addWidget(MakeLabel(apps_card, "Apps to add to your library", "muted"));
  app_tiles_ = new QWidget(apps_card);
  auto* grid = new QGridLayout(app_tiles_);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setSpacing(8);
  apps->addWidget(app_tiles_);
  layout->addWidget(apps_card);

  layout->addWidget(MakeLabel(this, "Next starts the install. It keeps going while you finish setting up.",
                              "muted"));
  layout->addWidget(Note(this, icons::Glyph::EyeSlash,
                         new QLabel("You sign in inside Word or Excel the first time you open one, "
                                    "like on Windows. Mira never sees your account.")));
  layout->addWidget(SetupLaterLine(
      this, "Not now? Microsoft 365's page in the sidebar has this same setup, any time."));
  layout->addStretch(1);

  api::GetLaunchersAsync(this, [this](LaunchersResult result) {
    if (!result.ok) return;
    for (const LauncherInfo& launcher : result.launchers) {
      if (launcher.id == "office") apps_ = launcher.apps;
    }
    ShowApps();
  });
  api::GetConfigAsync(this, [this](ConfigResult result) {
    if (!result.ok) return;
    plan_ = QString::fromStdString(result.values["launchers.office.plan"]);
    Enter();
  });
}

void OfficeSetupPage::ShowApps() {
  auto* grid = static_cast<QGridLayout*>(app_tiles_->layout());
  Clear(grid);
  for (std::size_t i = 0; i < apps_.size(); ++i) {
    const QString ref = QString::fromStdString(apps_[i].ref);
    const QString name = QString::fromStdString(apps_[i].name);
    QPushButton* tile = Tile(app_tiles_);
    tile->setChecked(context_.choices->office_apps.contains(ref));
    auto* inside = new QGridLayout(tile);
    inside->setContentsMargins(4, 8, 6, 8);
    auto* column = new QVBoxLayout();
    column->setSpacing(6);
    auto* doc = new QLabel(name.left(1), tile);
    doc->setObjectName("setup_doc");
    doc->setFixedSize(32, 40);
    doc->setAlignment(Qt::AlignCenter);
    doc->setStyleSheet(QString("background: %1;").arg(OfficeAppColor(ref).name()));
    column->addWidget(doc, 0, Qt::AlignHCenter);
    auto* label = new QLabel(name, tile);
    label->setAlignment(Qt::AlignCenter);
    column->addWidget(label);
    inside->addLayout(column, 0, 0);
    inside->addWidget(Tick(tile), 0, 0, Qt::AlignRight | Qt::AlignTop);
    FinishTile(tile);
    connect(tile, &QAbstractButton::toggled, this, [this, ref](bool on) {
      QStringList& picked = context_.choices->office_apps;
      picked.removeAll(ref);
      if (on) picked << ref;
    });
    grid->addWidget(tile, static_cast<int>(i) / 4, static_cast<int>(i) % 4);
  }
  for (int c = 0; c < 4; ++c) grid->setColumnStretch(c, 1);
}

void OfficeSetupPage::Enter() {
  if (plan_.isEmpty() && !OfficePlans().empty()) plan_ = OfficePlans().front().value;
  for (QAbstractButton* button : plans_->buttons()) {
    button->setChecked(button->property("plan").toString() == plan_);
  }
}

void OfficeSetupPage::Leave() {
  std::vector<std::string> refs;
  for (const LauncherApp& app : apps_) {
    if (context_.choices->office_apps.contains(QString::fromStdString(app.ref))) refs.push_back(app.ref);
  }
  // The plan picks the edition the apps install from, so it's saved before they're asked for.
  api::PatchConfigAsync(this, {{"launchers.office.plan", "a string", plan_.toStdString()}},
                        [work = context_.work, refs](PatchConfigResult) { work->AddOfficeApps(refs); });
}

// --- Done ------------------------------------------------------------------------------------

DonePage::DonePage(const SetupContext& context) : context_(context) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(36, 14, 36, 22);
  layout->setSpacing(10);
  auto* check = new QLabel(this);
  check->setObjectName("setup_check");
  check->setFixedSize(64, 64);
  check->setAlignment(Qt::AlignCenter);
  check->setPixmap(icons::For(icons::Glyph::CheckCircle, theme::Current().success)
                       .pixmap(QSize(34, 34), devicePixelRatioF()));
  layout->addWidget(check, 0, Qt::AlignHCenter);
  title_ = new QLabel(this);
  title_->setObjectName("setup_done_title");
  title_->setAlignment(Qt::AlignCenter);
  layout->addWidget(title_);
  auto* thanks = MakeLabel(this, "Thanks for downloading Mira! Your library opens next.", "muted");
  thanks->setAlignment(Qt::AlignCenter);
  layout->addWidget(thanks);
  layout->addSpacing(6);

  summary_ = SetupCard(this);
  auto* rows = new QVBoxLayout(summary_);
  rows->setContentsMargins(16, 8, 16, 8);
  rows->setSpacing(0);
  layout->addWidget(summary_);

  QFrame* switches = SetupCard(this);
  auto* switch_rows = new QVBoxLayout(switches);
  switch_rows->setContentsMargins(14, 4, 14, 4);
  switch_rows->setSpacing(0);
  login_ = new Switch(switches);
  login_->setChecked(context.prefs->start_on_login.value_or(true));
  QWidget* login_row = SetupRow(switches, "Start Mira when I log in (in the tray)",
                                "Opens to the tray, so games are ready when you are.", login_);
  switch_rows->addWidget(login_row);
  if (context.screen != setup::Screen::Desktop) {
    big_screen_ = new Switch(switches);
    big_screen_->setChecked(true);
    QWidget* row = SetupRow(
        switches, "Open Mira in big screen",
        "Mira full screen, made for a controller. Switch any time from the menu or the Mira button.",
        big_screen_, /*below=*/false,
        MakeLabel(switches,
                  context.screen == setup::Screen::SteamDeck ? QString::fromUtf8("\xc2\xb7 looks like a Steam Deck")
                                                             : QString::fromUtf8("\xc2\xb7 looks like a TV"),
                  "muted", false));
    switch_rows->addWidget(MakeDivider(switches, Qt::Horizontal));
    switch_rows->addWidget(row);
    // A login start opens big screen instead of the tray when that's on.
    auto* login_name = login_row->findChild<QLabel*>();
    const auto say_where = [login_name](bool big_screen) {
      login_name->setText(big_screen ? "Start Mira when I log in"
                                     : "Start Mira when I log in (in the tray)");
      login_name->setToolTip(big_screen ? "Opens in big screen, so games are ready when you are."
                                        : "Opens to the tray, so games are ready when you are.");
    };
    say_where(big_screen_->isChecked());
    connect(big_screen_, &QAbstractButton::toggled, this, say_where);
  }
  layout->addWidget(switches);

  auto* star = new QFrame(this);
  star->setObjectName("setup_star");
  auto* star_line = new QHBoxLayout(star);
  star_line->setContentsMargins(16, 14, 16, 14);
  star_line->setSpacing(14);
  auto* glyph = new QLabel(QString::fromUtf8("\xe2\x98\x85"), star);
  glyph->setObjectName("setup_star_glyph");
  star_line->addWidget(glyph);
  auto* star_text = new QVBoxLayout();
  star_text->setSpacing(1);
  star_text->addWidget(MakeLabel(star, "Enjoying Mira? A star on GitHub helps other people find it."));
  auto* link = new QLabel(
      "<a href='https://github.com/Mira-Launcher/Mira'>github.com/Mira-Launcher/Mira</a>", star);
  link->setObjectName("setup_link");
  link->setOpenExternalLinks(true);
  link->setTextInteractionFlags(Qt::TextBrowserInteraction);
  star_text->addWidget(link);
  star_line->addLayout(star_text, /*stretch=*/1);
  layout->addWidget(star);
  layout->addStretch(1);

  connect(context.work, &SetupWork::Changed, this, [this] {
    if (isVisible()) ShowSummary();
  });
  connect(context.services.library, &GameLibraryModel::Changed, this, [this] {
    if (isVisible()) ShowSummary();
  });
}

void DonePage::Enter() {
  const setup::Choices& choices = *context_.choices;
  title_->setText(choices.skipped_all ? "Mira is ready" : "You're all set");
  ShowSummary();
  if (!choices.games || choices.stores_skipped) return;
  for (const QString& store : choices.stores) {
    if (store == "steam") {
      api::GetConfigAsync(this, [this](ConfigResult result) {
        signed_in_["steam"] = result.ok && !result.values["steam.web_api_key"].empty();
        ShowSummary();
      });
      continue;
    }
    api::GetStoreStatusAsync(this, store.toStdString(), [this, store](StoreStatusResult status) {
      signed_in_[store] = status.ok && status.authenticated;
      ShowSummary();
    });
  }
}

void DonePage::ShowSummary() {
  auto* rows = static_cast<QVBoxLayout*>(summary_->layout());
  Clear(rows);
  const theme::Tokens& tokens = theme::Current();
  int added = 0;
  enum class Mark { Done, Left, Running };
  const auto add = [this, rows, &tokens, &added](Mark state, const QString& text,
                                                const QString& detail) {
    ++added;
    auto* row = new QWidget(summary_);
    auto* line = new QHBoxLayout(row);
    line->setContentsMargins(0, 6, 0, 6);
    line->setSpacing(10);
    auto* mark = new QLabel(state == Mark::Done      ? QString::fromUtf8("\xe2\x9c\x93")
                            : state == Mark::Running ? QString::fromUtf8("\xe2\x97\x8f")
                                                     : QString::fromUtf8("\xe2\x97\x8b"),
                            row);
    mark->setFixedSize(18, 18);
    mark->setAlignment(Qt::AlignCenter);
    const QColor color = state == Mark::Done      ? tokens.success
                         : state == Mark::Running ? tokens.accent
                                                  : tokens.text_muted;
    mark->setStyleSheet(QString("color: %1;").arg(color.name()));
    line->addWidget(mark, 0, Qt::AlignTop);
    auto* text_column = new QVBoxLayout();
    text_column->setSpacing(0);
    text_column->addWidget(MakeLabel(row, text));
    if (!detail.isEmpty()) text_column->addWidget(MakeLabel(row, detail, "muted"));
    line->addLayout(text_column, /*stretch=*/1);
    rows->addWidget(row);
  };

  const setup::Choices& choices = *context_.choices;
  const auto count = [this](const char* source) {
    return static_cast<int>(std::ranges::count_if(
        context_.services.library->Games(), [source](const GameSummary& g) { return g.source == source; }));
  };
  if (choices.games) {
    for (const auto& [source, name] : {std::pair{"steam", "Steam"}, std::pair{"lutris", "Lutris"}}) {
      const int games = count(source);
      if (games > 0) {
        add(Mark::Done, QString("%1: %2 games in your library").arg(name).arg(games), {});
      }
    }
  }
  if (choices.games && !choices.stores_skipped) {
    for (const QString& store : choices.stores) {
      if (!signed_in_.contains(store)) continue;
      if (store == "steam") {
        if (signed_in_[store]) {
          add(Mark::Done, "Steam: the games you own are listed", {});
        } else {
          add(Mark::Left, "Steam's games you own",
              "No key yet. Steam's page in the sidebar has this step.");
        }
      } else if (signed_in_[store]) {
        add(Mark::Done, StoreName(store) + ": signed in", {});
      } else {
        add(Mark::Left, StoreName(store), "Not signed in. Its page in the sidebar has the sign-in.");
      }
    }
  }
  if (choices.apps && choices.office && !choices.apps_skipped) {
    const SetupWork* work = context_.work;
    QString running;
    for (const QString& line : work->Running()) {
      if (line.startsWith("Microsoft 365")) running = line;
    }
    if (!running.isEmpty()) {
      // "Microsoft 365 · 34%": the share done, when the installer says.
      const QString done = running.section(QString::fromUtf8(" \xc2\xb7 "), 1);
      add(Mark::Running, "Microsoft 365 is installing",
          (done.isEmpty() ? QString() : done + " done. ") +
              "It keeps going after this window closes.");
    } else if (work->OfficeFailed()) {
      add(Mark::Left, "Microsoft 365", "Setting it up failed. Its page in the sidebar shows why.");
    } else if (work->OfficeStarted()) {
      add(Mark::Done, "Microsoft 365: installed", {});
    } else {
      add(Mark::Left, "Microsoft 365", "Not installed. Its page in the sidebar has the setup.");
    }
  }
  // Rows made just now aren't shown yet, which QLayout::isEmpty counts as empty.
  if (added == 0) {
    auto* none = MakeLabel(summary_,
                           "Nothing was set up, and that's fine. Add games and sign in to stores "
                           "from the sidebar any time.",
                           "muted");
    none->setTextFormat(Qt::RichText);
    none->setContentsMargins(0, 6, 0, 6);
    rows->addWidget(none);
  }
}

bool DonePage::BigScreen() const { return big_screen_ != nullptr && big_screen_->isChecked(); }

void DonePage::Leave() {
  FrontendPrefs change;
  change.start_on_login = login_->isChecked();
  if (big_screen_ != nullptr) change.big_screen_at_start = big_screen_->isChecked();
  SaveSetupPrefs(context_, change);
  bigscreen::ApplyStartOnLogin(login_->isChecked());
}

}  // namespace mira_gui
