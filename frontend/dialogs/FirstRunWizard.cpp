#include "FirstRunWizard.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLibrary>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "../client/api/Config.h"
#include "../client/api/Runners.h"
#include "../sources/Sources.h"
#include "../widgets/Labels.h"

namespace mira_gui {
namespace {

QString Home(const QString& rest) { return QDir::homePath() + "/" + rest; }

bool SteamInstalled() {
  return QFileInfo::exists(Home(".steam/steam")) || QFileInfo::exists(Home(".local/share/Steam"));
}

bool LutrisInstalled() {
  return QFileInfo::exists(Home(".local/share/lutris")) || !QStandardPaths::findExecutable("lutris").isEmpty();
}

QRadioButton* Choice(QWidget* parent, QVBoxLayout* layout, QButtonGroup* group, const QString& key,
                     const QString& title, const QString& detail) {
  auto* button = new QRadioButton(title, parent);
  button->setProperty("key", key);
  group->addButton(button);
  layout->addWidget(button);
  auto* note = MakeLabel(parent, detail, "muted");
  note->setContentsMargins(24, 0, 0, 8);
  layout->addWidget(note);
  return button;
}

QString Checked(const QButtonGroup* group) {
  const QAbstractButton* button = group->checkedButton();
  return button != nullptr ? button->property("key").toString() : QString();
}

QWidget* MakePage(const QString& title, const QString& intro, QVBoxLayout*& body) {
  auto* page = new QWidget();
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  layout->addWidget(MakeLabel(page, title, "heading"));
  if (!intro.isEmpty()) layout->addWidget(MakeLabel(page, intro, "muted"));
  layout->addSpacing(8);
  body = layout;
  return page;
}

}  // namespace

bool FirstRunWizard::Needed(const FrontendPrefs& prefs) {
  // A frontend.toml that already has a window size is an existing install, not a new one.
  return !prefs.onboarded.value_or(false) && !prefs.window_width.has_value();
}

FirstRunWizard::FirstRunWizard(const FrontendPrefs& prefs, QWidget* parent) : QDialog(parent), initial_(prefs) {
  setWindowTitle("Welcome to Mira");
  setModal(true);
  resize(720, 560);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(32, 24, 32, 20);
  step_ = MakeLabel(this, QString(), "muted");
  root->addWidget(step_);
  pages_ = new QStackedWidget(this);
  for (QWidget* page : {BuildWelcome(), BuildUse(), BuildFolder(), BuildSources(), BuildRunner(), BuildArt(),
                        BuildSidebar(), BuildCheck(), BuildSummary()}) {
    pages_->addWidget(page);
  }
  root->addWidget(pages_, 1);
  error_ = MakeLabel(this, QString(), "error");
  error_->setVisible(false);
  root->addWidget(error_);

  auto* buttons = new QHBoxLayout();
  skip_ = new QPushButton("Skip setup", this);
  skip_->setObjectName("text_button");
  connect(skip_, &QPushButton::clicked, this, &FirstRunWizard::Skip);
  back_ = new QPushButton("Back", this);
  connect(back_, &QPushButton::clicked, this, [this] { Go(-1); });
  next_ = new QPushButton("Next", this);
  next_->setDefault(true);
  connect(next_, &QPushButton::clicked, this, [this] { Go(1); });
  buttons->addWidget(skip_);
  buttons->addStretch(1);
  buttons->addWidget(back_);
  buttons->addWidget(next_);
  root->addLayout(buttons);

  PreselectSources();
  connect(use_, &QButtonGroup::buttonClicked, this, [this] { PreselectSources(); });
  // Only offered a runner when there is none.
  api::ListRunnersAsync(this, [this](RunnersResult result) {
    if (result.ok) have_runners_ = !result.runners.empty();
  });
  ShowPage(Welcome);
}

QWidget* FirstRunWizard::BuildWelcome() {
  QVBoxLayout* body = nullptr;
  QWidget* page = MakePage("Welcome to Mira",
                       "Mira keeps your games and applications in one library and runs them with Wine or "
                       "Proton. A few questions first, about a minute. Everything can be changed later in Settings.",
                       body);
  body->addStretch(1);
  return page;
}

QWidget* FirstRunWizard::BuildUse() {
  QVBoxLayout* body = nullptr;
  QWidget* page = MakePage("What will you mostly use Mira for?",
                       "This only sets what you see first. Nothing is hidden for good.", body);
  use_ = new QButtonGroup(page);
  Choice(page, body, use_, "games", "Games",
         "Steam, Epic and GOG are suggested.");
  Choice(page, body, use_, "apps", "Applications",
         "The library opens on Apps. Microsoft 365 and Lutris are suggested.");
  Choice(page, body, use_, "both", "Both", "Everything together, as it comes.");
  const QString use = QString::fromStdString(initial_.primary_use.value_or("both"));
  for (QAbstractButton* button : use_->buttons()) button->setChecked(button->property("key").toString() == use);
  body->addStretch(1);
  return page;
}

QWidget* FirstRunWizard::BuildFolder() {
  QVBoxLayout* body = nullptr;
  QWidget* page = MakePage("Where should Mira keep things?",
                       "Mira takes over one folder. It creates Games, Applications and prefixes inside it, "
                       "watches Games and Applications for anything you drop in, and keeps each program's "
                       "Wine prefix under prefixes.",
                       body);
  folder_ = new QButtonGroup(page);
  Choice(page, body, folder_, "mira", "Use ~/Mira", "The default.");
  Choice(page, body, folder_, "custom", "Choose another folder", "Games, Applications and prefixes go inside it.");
  auto* row = new QHBoxLayout();
  folder_path_ = new QLineEdit(page);
  folder_path_->setPlaceholderText("Folder");
  auto* browse = new QPushButton("Browse…", page);
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString dir = QFileDialog::getExistingDirectory(this, "Mira folder", QDir::homePath());
    if (dir.isEmpty()) return;
    folder_path_->setText(dir);
    for (QAbstractButton* button : folder_->buttons()) {
      if (button->property("key") == "custom") button->setChecked(true);
    }
  });
  row->addSpacing(24);
  row->addWidget(folder_path_, 1);
  row->addWidget(browse);
  body->addLayout(row);
  body->addSpacing(8);
  Choice(page, body, folder_, "none", "Don't manage a folder",
         "Mira watches nothing. Add games by hand or through a source. Prefixes go in Mira's own data folder.");
  folder_->buttons().first()->setChecked(true);
  body->addStretch(1);
  return page;
}

QWidget* FirstRunWizard::BuildSources() {
  QVBoxLayout* body = nullptr;
  QWidget* page = MakePage("Which sources do you use?",
                       "Sources show up in the sidebar. Signing in to a store happens on its own page afterwards.",
                       body);
  for (const SourceInfo& source : AllSources()) {
    auto* box = new QCheckBox(source.name, page);
    sources_[source.id] = box;
    body->addWidget(box);
  }
  body->addSpacing(8);
  body->addWidget(MakeLabel(page, "Steam lists the games you own, not only installed ones, with a Web API key "
                                  "and your SteamID64. Both are optional.", "muted"));
  const auto field = [&](const QString& label, const QString& hint, bool secret) {
    auto* edit = new QLineEdit(page);
    edit->setPlaceholderText(hint);
    if (secret) edit->setEchoMode(QLineEdit::Password);
    auto* row = new QHBoxLayout();
    auto* name = new QLabel(label, page);
    name->setMinimumWidth(130);
    row->addWidget(name);
    row->addWidget(edit, 1);
    body->addLayout(row);
    return edit;
  };
  steam_root_ = field("Steam folder", "Detected automatically", false);
  steam_key_ = field("Steam Web API key", "steamcommunity.com/dev/apikey", true);
  steam_id_ = field("SteamID64", "17 digits", false);
  body->addStretch(1);
  return page;
}

QWidget* FirstRunWizard::BuildRunner() {
  QVBoxLayout* body = nullptr;
  QWidget* page = MakePage("A runner for Windows programs",
                       "Mira found no Proton or Wine build. One is needed to run Windows games and applications.",
                       body);
  get_runner_ = new QCheckBox("Download the latest Proton-GE when setup finishes", page);
  get_runner_->setChecked(true);
  body->addWidget(get_runner_);
  runner_note_ = MakeLabel(page, "You can add or change builds later under Runners.", "muted");
  body->addWidget(runner_note_);
  body->addStretch(1);
  return page;
}

QWidget* FirstRunWizard::BuildArt() {
  QVBoxLayout* body = nullptr;
  QWidget* page = MakePage("Cover art and details",
                       "Mira can fetch covers, descriptions and ProtonDB ratings when it finds a game. Steam games "
                       "need no key. Other games need a free SteamGridDB key for covers.",
                       body);
  metadata_ = new QCheckBox("Fetch metadata automatically", page);
  metadata_->setChecked(true);
  body->addWidget(metadata_);
  art_key_ = new QLineEdit(page);
  art_key_->setPlaceholderText("SteamGridDB API key (optional)");
  art_key_->setEchoMode(QLineEdit::Password);
  body->addWidget(art_key_);
  body->addWidget(MakeLabel(page, "Get a key at steamgriddb.com/profile/preferences/api", "muted"));
  body->addStretch(1);
  return page;
}

QWidget* FirstRunWizard::BuildSidebar() {
  QVBoxLayout* body = nullptr;
  QWidget* page = MakePage("The sidebar",
                       "How pinned and recently played games look. You pin games later from their menu.", body);
  const auto styles = [&](const QString& label, const QString& current) {
    auto* combo = new QComboBox(page);
    combo->addItem("Covers", "covers");
    combo->addItem("Hero banners", "hero");
    combo->addItem("Shelf", "shelf");
    combo->setCurrentIndex(std::max(0, combo->findData(current)));
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(label, page), 1);
    row->addWidget(combo);
    body->addLayout(row);
    return combo;
  };
  pinned_style_ = styles("Pinned style", QString::fromStdString(initial_.sidebar_pinned_style.value_or("covers")));
  recent_style_ = styles("Recently played style", QString::fromStdString(initial_.sidebar_recent_style.value_or("covers")));
  recent_count_ = new QSpinBox(page);
  recent_count_->setRange(0, 10);
  recent_count_->setValue(initial_.sidebar_recent_count.value_or(5));
  recent_count_->setSpecialValueText("Hidden");
  auto* row = new QHBoxLayout();
  row->addWidget(new QLabel("Recently played shown", page), 1);
  row->addWidget(recent_count_);
  body->addLayout(row);
  body->addStretch(1);
  return page;
}

QWidget* FirstRunWizard::BuildCheck() {
  QVBoxLayout* body = nullptr;
  QWidget* page = MakePage("System check", "What Mira found on this computer.", body);
  check_ = MakeLabel(page, "Checking…", nullptr);
  check_->setTextFormat(Qt::RichText);
  body->addWidget(check_);
  body->addStretch(1);
  return page;
}

QWidget* FirstRunWizard::BuildSummary() {
  QVBoxLayout* body = nullptr;
  QWidget* page = MakePage("Ready", "Here is what will be set. Back changes any of it.", body);
  summary_ = MakeLabel(page, QString(), nullptr);
  summary_->setTextFormat(Qt::RichText);
  body->addWidget(summary_);
  body->addStretch(1);
  return page;
}

QString FirstRunWizard::PrimaryUse() const { return Checked(use_); }

void FirstRunWizard::PreselectSources() {
  QStringList ids;
  const QString use = PrimaryUse();
  if (use == "apps") {
    ids = {"office", "lutris"};
  } else if (use == "games") {
    ids = {"steam", "epic", "gog"};
  } else {
    ids = {"steam", "epic", "gog", "office"};
  }
  if (SteamInstalled()) ids << "steam";
  if (LutrisInstalled()) ids << "lutris";
  for (const auto& [id, box] : sources_) box->setChecked(ids.contains(id));
  if (SteamInstalled() && steam_root_ != nullptr) {
    steam_root_->setPlaceholderText(QFileInfo::exists(Home(".steam/steam")) ? "~/.steam/steam" : "~/.local/share/Steam");
  }
}

void FirstRunWizard::ShowPage(int page) {
  pages_->setCurrentIndex(page);
  step_->setText(QString("Step %1 of %2").arg(page + 1).arg(PageCount));
  back_->setEnabled(page != Welcome);
  next_->setText(page == Summary ? "Finish" : "Next");
  skip_->setVisible(page != Summary);
  if (page == Check) FillCheck();
  if (page == Summary) summary_->setText(SummaryText());
}

void FirstRunWizard::Go(int delta) {
  if (saving_) return;
  int page = pages_->currentIndex() + delta;
  if (page == Runner && have_runners_) page += delta;  // nothing to ask when one is installed
  if (page >= PageCount) return Finish();
  if (page < 0) return;
  ShowPage(page);
}

void FirstRunWizard::FillCheck() {
  check_->setText("Checking…");
  const auto line = [](bool ok, const QString& text) {
    return QString("<span style=\"color:%1\">●</span> %2<br>").arg(ok ? "#4caf50" : "#f0883e", text);
  };
  const bool vulkan = QLibrary("vulkan", 1).load();
  const QString base = line(vulkan, vulkan ? "Vulkan is installed." : "Vulkan is missing. Install your distribution's vulkan loader and driver packages; most games need it.") +
                       line(have_runners_, have_runners_ ? "A Proton or Wine build is installed." : "No Proton or Wine build yet.");
  check_->setText(base);
  api::GetGameModeStatusAsync(this, [this, base, line](GameModeStatusResult result) {
    if (!result.ok) return;
    check_->setText(base + line(result.installed, result.installed ? "GameMode is available." : "GameMode is not installed. Optional; it can raise performance."));
  });
}

QString FirstRunWizard::SummaryText() const {
  const QString use = PrimaryUse();
  QStringList enabled;
  for (const auto& [id, box] : sources_) {
    if (box->isChecked()) enabled << FindSourceInfo(id)->name;
  }
  const QString folder = Checked(folder_);
  QString where = folder == "none" ? "none managed" : folder == "custom" && !folder_path_->text().isEmpty() ? folder_path_->text() : "~/Mira";
  return QString("<b>Use</b>: %1<br><b>Folder</b>: %2<br><b>Sources</b>: %3<br><b>Metadata</b>: %4<br>")
      .arg(use == "games" ? "Games" : use == "apps" ? "Applications" : "Both", where.toHtmlEscaped(),
           enabled.isEmpty() ? "none" : enabled.join(", ").toHtmlEscaped(), metadata_->isChecked() ? "on" : "off");
}

std::vector<ConfigEdit> FirstRunWizard::Edits() const {
  std::vector<ConfigEdit> edits;
  const auto add = [&](const QString& key, const char* type, const QString& value) {
    edits.push_back({key.toStdString(), type, value.toStdString()});
  };
  const auto list = [](const QStringList& items) {
    QString text = "[";
    for (int i = 0; i < items.size(); ++i) {
      text += (i > 0 ? "," : "") + QString("\"%1\"").arg(QString(items[i]).replace("\\", "\\\\").replace("\"", "\\\""));
    }
    return text + "]";
  };
  const QString folder = Checked(folder_);
  if (folder == "none") {
    add("library_roots", "an array of strings", "[]");
    add("prefix_root", "a string", QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/mira/prefixes");
  } else if (folder == "custom" && !folder_path_->text().trimmed().isEmpty()) {
    const QString dir = QDir::cleanPath(folder_path_->text().trimmed());
    add("library_roots", "an array of strings", list({dir + "/Games", dir + "/Applications"}));
    add("prefix_root", "a string", dir + "/prefixes");
  }
  for (const auto& [id, box] : sources_) add(id + ".enabled", "a boolean", box->isChecked() ? "true" : "false");
  if (!steam_root_->text().trimmed().isEmpty()) add("steam.root", "a string", steam_root_->text().trimmed());
  if (!steam_key_->text().trimmed().isEmpty()) add("steam.web_api_key", "a string", steam_key_->text().trimmed());
  if (!steam_id_->text().trimmed().isEmpty()) add("steam.steamid64", "a string", steam_id_->text().trimmed());
  add("metadata.enabled", "a boolean", metadata_->isChecked() ? "true" : "false");
  if (!art_key_->text().trimmed().isEmpty()) add("steamgriddb.api_key", "a string", art_key_->text().trimmed());
  return edits;
}

FrontendPrefs FirstRunWizard::Prefs() const {
  FrontendPrefs prefs;
  prefs.onboarded = true;
  prefs.primary_use = PrimaryUse().toStdString();
  if (PrimaryUse() == "apps") prefs.library_filter = "apps";
  prefs.sidebar_pinned_style = pinned_style_->currentData().toString().toStdString();
  prefs.sidebar_recent_style = recent_style_->currentData().toString().toStdString();
  prefs.sidebar_recent_count = recent_count_->value();
  return prefs;
}

void FirstRunWizard::Finish() {
  saving_ = true;
  next_->setEnabled(false);
  error_->setVisible(false);
  const auto fail = [this](const QString& what) {
    saving_ = false;
    next_->setEnabled(true);
    error_->setText(what);
    error_->setVisible(true);
  };
  api::PatchConfigAsync(this, Edits(), [this, fail](PatchConfigResult result) {
    if (!result.ok) return fail("Could not save the settings: " + QString::fromStdString(result.error.message));
    api::SaveFrontendPrefsAsync(this, Prefs(), [this, fail](PatchConfigResult saved) {
      if (!saved.ok) return fail("Could not save your choices: " + QString::fromStdString(saved.error.message));
      done_ = true;
      if (get_runner_ != nullptr && !have_runners_ && get_runner_->isChecked()) {
        // Large; it carries on in the background and the Runners page shows it.
        api::GetRunnerCatalogAsync(qApp, "proton", "", [](RunnerCatalogResult catalog) {
          if (!catalog.ok || catalog.releases.empty()) return;
          const RunnerRelease& release = catalog.releases.front();
          api::DownloadRunnerAsync(qApp, "proton", release.tag, release.source, [](RunnerDownloadResult) {});
        });
      }
      accept();
    });
  });
}

// Leaving it any other way still counts: it asks once.
void FirstRunWizard::Skip() { reject(); }

void FirstRunWizard::reject() {
  if (saving_) return;
  if (!done_) {
    FrontendPrefs prefs;
    prefs.onboarded = true;
    api::SaveFrontendPrefsBlocking(prefs);
  }
  QDialog::reject();
}

}  // namespace mira_gui
