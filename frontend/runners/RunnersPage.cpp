#include "RunnersPage.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "../client/EventHub.h"
#include "../client/api/Config.h"
#include "../client/api/Runners.h"
#include "../activity/DownloadTracker.h"
#include "../app/Notify.h"
#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui {
namespace {

constexpr const char* kDefaultKey = "default_runner.windows";

QString FormatSize(std::int64_t bytes) {
  if (bytes <= 0) return QString();
  const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
  return mib >= 1024.0 ? QString("%1 GiB").arg(mib / 1024.0, 0, 'f', 1) : QString("%1 MiB").arg(mib, 0, 'f', 0);
}

QString FormatDate(const std::string& iso) {
  const QDateTime when = QDateTime::fromString(QString::fromStdString(iso), Qt::ISODate);
  return when.isValid() ? QLocale().toString(when.date(), "MMM d, yyyy") : QString();
}

QLabel* Muted(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "muted");
  label->setWordWrap(true);
  return label;
}

// A row's facts after its name, in one line that shrinks before the controls do.
ElidedLabel* Facts(const QString& text, SettingRow* row) {
  auto* facts = new ElidedLabel(text, row);
  facts->setProperty("role", "subtle");
  row->AddAfterLabel(facts);
  return facts;
}

// An empty list's one line, in the card's row padding.
SettingRow* NoteRow(const QString& text) {
  auto* row = new SettingRow(text, {});
  row->Label()->setProperty("role", "subtle");
  return row;
}

QProgressBar* Busy(QWidget* parent) {
  auto* progress = new QProgressBar(parent);
  progress->setRange(0, 0);
  progress->setTextVisible(false);
  progress->setFixedSize(90, 4);
  return progress;
}

}  // namespace

RunnersPage::RunnersPage(DownloadTracker* downloads, QWidget* parent) : QWidget(parent), downloads_(downloads) {
  setObjectName("runners_page");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* header_box = new QWidget(this);
  header_box->setObjectName("settings_canvas");
  auto* header = new QHBoxLayout(header_box);
  header->setContentsMargins(32, 22, 32, 6);
  auto* title = new QLabel("Runners", this);
  title->setObjectName("page_title");
  header->addWidget(title, /*stretch=*/1);
  // Only the two kinds with builds to download. `native` has none, and
  // `steam` resolves its build per game from Steam's own prefix.
  auto* segmented = new QWidget(this);
  segmented->setObjectName("segmented");
  auto* segmented_layout = new QHBoxLayout(segmented);
  segmented_layout->setContentsMargins(3, 3, 3, 3);
  segmented_layout->setSpacing(3);
  kinds_ = new QButtonGroup(this);
  for (const auto& [label, kind] : {std::pair{"Proton", "proton"}, std::pair{"Wine", "wine"}}) {
    auto* button = new QPushButton(label, segmented);
    button->setCheckable(true);
    button->setProperty("kind", kind);
    kinds_->addButton(button);
    segmented_layout->addWidget(button);
  }
  kinds_->buttons().first()->setChecked(true);
  connect(kinds_, &QButtonGroup::buttonClicked, this, [this] {
    catalog_loaded_ = false;
    releases_.clear();
    RebuildInstalled();
    RefreshSources();
    RebuildTools();
  });
  header->addWidget(segmented);
  outer->addWidget(header_box);

  // Both columns scroll together, each as tall as its own cards.
  auto* scroll = new QScrollArea(this);
  scroll->setObjectName("settings_page");
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* canvas = new QWidget();
  canvas->setObjectName("settings_canvas");
  columns_ = new QBoxLayout(QBoxLayout::LeftToRight, canvas);
  QBoxLayout* columns = columns_;
  columns->setContentsMargins(32, 12, 32, 22);
  columns->setSpacing(18);
  scroll->setWidget(canvas);
  outer->addWidget(scroll, /*stretch=*/1);
  auto* left = new QVBoxLayout();
  left->setSpacing(14);
  auto* right = new QVBoxLayout();
  right->setSpacing(14);
  // Each column as tall as its cards; the spacer takes what's left once stacked.
  columns->addLayout(left, /*stretch=*/1);
  columns->addLayout(right, /*stretch=*/1);
  columns->setAlignment(left, Qt::AlignTop);
  columns->setAlignment(right, Qt::AlignTop);
  columns->addStretch(0);

  // What runners need besides a build, first since nothing works without it.
  tools_card_ = new SettingsCard("Missing tools", canvas);
  tools_card_->hide();
  left->addWidget(tools_card_);

  installed_ = new SettingsCard("Installed", canvas);
  left->addWidget(installed_);
  default_note_ = Muted(QString(), canvas);
  default_note_->setContentsMargins(4, 0, 4, 0);
  default_note_->setTextFormat(Qt::RichText);
  connect(default_note_, &QLabel::linkActivated, this, [this] {
    api::ResetConfigKeyAsync(this, kDefaultKey, [this](PatchConfigResult result) {
      if (!result.ok) {
        SetStatus("Could not reset the default: " + error_help::Describe(result.error), true);
        return;
      }
      RefreshInstalled();
    });
  });
  left->addWidget(default_note_);

  catalog_ = new SettingsCard("Get more", canvas);
  source_ = new QComboBox(catalog_);
  source_->setToolTip("Where builds are downloaded from");
  source_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  connect(source_, &QComboBox::activated, this, [this] { RefreshCatalog(); });
  catalog_->Header()->addWidget(source_);
  auto* refresh = new QToolButton(catalog_);
  refresh->setAutoRaise(true);
  refresh->setIcon(icons::For(icons::Glyph::Refresh, theme::Current().text_muted));
  refresh->setToolTip("Check again");
  connect(refresh, &QToolButton::clicked, this, &RunnersPage::Refresh);
  catalog_->Header()->addWidget(refresh);
  right->addWidget(catalog_);
  status_ = Muted(QString(), canvas);
  status_->setContentsMargins(4, 0, 4, 0);
  right->addWidget(status_);

  connect(downloads_, &DownloadTracker::Changed, this, &RunnersPage::DownloadChanged);
  // Removed here, from the CLI or by another window.
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string&, bool live) {
    if (!live || type != "runners.removed") return;
    RefreshInstalled();
    RefreshUpdates();
    RefreshCatalog();
  });
  Refresh();
}

void RunnersPage::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  // Below this each column is too narrow for a build's facts beside its buttons.
  constexpr int kTwoColumnWidth = 1150;
  const bool wide = width() >= kTwoColumnWidth;
  columns_->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
  columns_->setStretch(0, wide ? 1 : 0);
  columns_->setStretch(1, wide ? 1 : 0);
}

void RunnersPage::SetGames(const std::vector<GameSummary>& games) {
  games_ = games;
  RebuildInstalled();
}

std::string RunnersPage::CurrentKind() const {
  return kinds_->checkedButton()->property("kind").toString().toStdString();
}

void RunnersPage::SetStatus(const QString& text, bool error) {
  status_->setText(text);
  theme::SetStyleProperty(status_, "role", error ? "error" : "muted");
}

void RunnersPage::Refresh() {
  RefreshInstalled();
  RefreshSources();
  RefreshUpdates();
  RefreshTools();
}

void RunnersPage::RefreshSources() {
  const std::string kind = CurrentKind();
  const auto fill = [this] {
    const std::string kind = CurrentKind();
    const QString chosen = source_->currentData().toString();
    source_->clear();
    for (const RunnerSourceInfo& info : sources_[kind]) {
      source_->addItem(QString::fromStdString(info.label), QString::fromStdString(info.id));
    }
    if (const int at = source_->findData(chosen); at >= 0) source_->setCurrentIndex(at);
    RefreshCatalog();
  };
  if (sources_.contains(kind)) return fill();
  api::ListRunnerSourcesAsync(this, kind, [this, kind, fill](RunnerSourcesResult result) {
    if (!result.ok) {
      SetStatus("Could not list sources: " + error_help::Describe(result.error), true);
      return;
    }
    sources_[kind] = result.sources;
    if (kind == CurrentKind()) fill();
  });
}

void RunnersPage::RefreshUpdates() {
  api::GetRunnerUpdatesAsync(this, [this](RunnerUpdatesResult result) {
    if (!result.ok) return;  // offline; the catalog says so
    updates_ = result.updates;
    RebuildInstalled();
  });
}

void RunnersPage::RefreshTools() {
  api::ListRunnerToolsAsync(this, [this](RunnerToolsResult result) {
    if (!result.ok) return;
    tools_ = result.tools;
    RebuildTools();
  });
}

QString RunnersPage::SourceLabel(const std::string& id) const {
  for (const auto& [kind, list] : sources_) {
    for (const RunnerSourceInfo& info : list) {
      if (info.id == id) return QString::fromStdString(info.label);
    }
  }
  return QString();
}

void RunnersPage::RefreshInstalled() {
  api::GetConfigAsync(this, [this](ConfigResult result) {
    if (!result.ok) return;
    const auto found = result.values.find(kDefaultKey);
    default_windows_ = found != result.values.end() ? found->second : std::string();
    RebuildInstalled();
  });
  api::ListRunnersAsync(this, [this](RunnersResult result) {
    if (!result.ok) {
      SetStatus(error_help::Describe(result.error), true);
      return;
    }
    runners_ = result.runners;
    RebuildInstalled();
  });
}

void RunnersPage::RefreshCatalog() {
  catalog_loaded_ = false;
  RebuildCatalog();
  // The one call that leaves the machine; say so rather than look hung.
  SetStatus("Checking for builds…");
  const std::string kind = CurrentKind();
  const std::string source = source_->currentData().toString().toStdString();
  api::GetRunnerCatalogAsync(this, kind, source, [this, kind, source](RunnerCatalogResult result) {
    if (kind != CurrentKind() || source != source_->currentData().toString().toStdString()) return;
    if (!result.ok) {
      SetStatus("Could not list builds: " + error_help::Describe(result.error), true);
      return;
    }
    releases_ = result.releases;
    catalog_loaded_ = true;
    SetStatus(QString());
    RebuildCatalog();
  });
}

void RunnersPage::RebuildInstalled() {
  installed_->ClearRows();
  const std::string kind = CurrentKind();
  const theme::Tokens& tokens = theme::Current();
  int shown = 0;
  for (const RunnerInfo& runner : runners_) {
    if (runner.kind != kind) continue;
    ++shown;
    const bool is_default = runner.reference == default_windows_;
    const auto used = std::ranges::count(games_, runner.reference, &GameSummary::runner_ref);

    auto* row = new SettingRow(QString::fromStdString(runner.label), {});
    if (is_default) {
      auto* badge = new QLabel("Default", row);
      badge->setObjectName("default_tag");
      row->AddAfterLabel(badge);
    }

    QStringList facts;
    facts << (used == 1 ? QString("Used by 1 game") : QString("Used by %1 games").arg(used));
    // Some builds only report a bare build timestamp, which means nothing here.
    const QString version = QString::fromStdString(runner.version);
    bool numeric = false;
    version.toLongLong(&numeric);
    if (!version.isEmpty() && !numeric && runner.version != runner.name) facts << version;
    if (const QString source = SourceLabel(runner.source); !source.isEmpty()) facts << source;
    // Not Mira's to remove or update: the distro's, Steam's or the system's.
    if (!runner.removable) facts << "Managed outside Mira";
    Facts(facts.join(" · "), row);

    const auto update = std::ranges::find(updates_, runner.reference, &RunnerUpdate::reference);
    if (update != updates_.end()) {
      const DownloadTracker::Entry* entry = downloads_->Find(DownloadTracker::KeyFor(
          DownloadTracker::Kind::Runner, QString::fromStdString(kind), QString::fromStdString(update->name)));
      if (entry != nullptr && entry->state == DownloadTracker::State::Running) {
        auto* updating = new QLabel("Updating to " + QString::fromStdString(update->label) + "…", row);
        updating->setProperty("role", "subtle");
        row->AddControl(updating);
      } else {
        // Short, so a long build name can't crowd out the row's facts.
        auto* update_button = new QPushButton("Update", row);
        update_button->setToolTip("Update to " + QString::fromStdString(update->label));
        update_button->setDefault(true);
        connect(update_button, &QPushButton::clicked, this, [this, update_button, runner, u = *update] {
          update_button->setEnabled(false);
          Update(runner, u);
        });
        row->AddControl(update_button);
      }
    }

    auto* more = new QToolButton(row);
    more->setAutoRaise(true);
    more->setIcon(icons::For(icons::Glyph::More));
    more->setToolTip("More");
    QSizePolicy keep = more->sizePolicy();
    keep.setRetainSizeWhenHidden(true);
    more->setSizePolicy(keep);
    more->setVisible(!is_default || runner.removable);
    connect(more, &QToolButton::clicked, this, [this, more, runner, is_default] {
      QMenu menu(this);
      if (!is_default) {
        menu.addAction("Make default", this, [this, reference = runner.reference] { SetDefault(reference); });
      }
      if (runner.removable) {
        if (!is_default) menu.addSeparator();
        menu.addAction("Remove…", this, [this, runner] { Remove(runner); });
      }
      menu.exec(more->mapToGlobal(QPoint(0, more->height())));
    });
    row->AddControl(more);
    installed_->AddRow(row);
  }
  if (shown == 0) {
    installed_->AddRow(NoteRow(kind == "proton" ? "No Proton builds yet. Install one from Get more."
                                                : "No Wine builds yet. Install one from Get more."));
  }

  const bool automatic = default_windows_.empty() || default_windows_ == "auto";
  // "proton:GE-Proton11-7" reads as "GE-Proton11-7".
  const QString chosen = QString::fromStdString(default_windows_).section(':', 1);
  default_note_->setText(
      automatic ? "Windows games set to Auto use the newest Proton build, or Wine if there's none."
                : QString("Windows games set to Auto use %1. <a href=\"auto\" style=\"color:%2\">Pick "
                          "automatically instead</a>")
                      .arg(chosen.toHtmlEscaped(), tokens.accent.name()));
}

void RunnersPage::RebuildCatalog() {
  catalog_->ClearRows();
  if (!catalog_loaded_) return;
  const std::string kind = CurrentKind();
  const theme::Tokens& tokens = theme::Current();
  for (const RunnerRelease& release : releases_) {
    auto* row = new SettingRow(QString::fromStdString(release.label), {});
    QStringList facts;
    if (const QString date = FormatDate(release.published_at); !date.isEmpty()) facts << date;
    if (const QString size = FormatSize(release.size_bytes); !size.isEmpty()) facts << size;
    // Installs either way; it just can't be verified first, which is worth
    // knowing before a 500 MB download.
    facts << (release.has_checksum ? "verified" : "no checksum");
    ElidedLabel* details = Facts(facts.join(" · "), row);
    if (!release.has_checksum) details->setStyleSheet(QString("color: %1;").arg(tokens.warning.name()));

    const DownloadTracker::Entry* entry = downloads_->Find(DownloadTracker::KeyFor(
        DownloadTracker::Kind::Runner, QString::fromStdString(kind), QString::fromStdString(release.name)));
    const bool downloading = entry != nullptr && entry->state == DownloadTracker::State::Running;
    if (downloading) {
      row->AddControl(Busy(row));
    } else if (release.installed) {
      auto* label = new QLabel("Installed", row);
      label->setProperty("role", "subtle");
      row->AddControl(label);
    } else {
      auto* install = new QPushButton("Install", row);
      install->setDefault(true);
      install->setFixedWidth(90);
      connect(install, &QPushButton::clicked, this, [this, install, tag = release.tag] {
        install->setEnabled(false);  // until the tracker hears it started
        Download(tag);
      });
      row->AddControl(install);
    }
    catalog_->AddRow(row);
  }
  if (releases_.empty()) catalog_->AddRow(NoteRow("No builds found."));
}

void RunnersPage::RebuildTools() {
  tools_card_->ClearRows();
  for (const RunnerTool& tool : tools_) {
    if (tool.installed) continue;
    // umu only matters to Proton, winetricks to both.
    if (tool.id == "umu" && CurrentKind() != "proton") continue;
    auto* row = new SettingRow(QString::fromStdString(tool.label), QString::fromStdString(tool.doc));
    const DownloadTracker::Entry* entry = downloads_->Find(
        DownloadTracker::KeyFor(DownloadTracker::Kind::Tool, QString::fromStdString(tool.id), QString()));
    if (entry != nullptr && entry->state == DownloadTracker::State::Running) {
      row->AddControl(Busy(row));
    } else {
      auto* install = new QPushButton("Install", row);
      install->setDefault(true);
      connect(install, &QPushButton::clicked, this, [this, install, tool] {
        install->setEnabled(false);
        SetupTool(tool);
      });
      row->AddControl(install);
    }
    tools_card_->AddRow(row);
  }
  tools_card_->setVisible(!tools_card_->Rows().isEmpty());
}

void RunnersPage::Update(const RunnerInfo& runner, const RunnerUpdate& update) {
  replacing_[update.name] = runner.reference;
  api::UpdateRunnerAsync(this, runner.reference, [this, name = update.name](RunnerDownloadResult result) {
    if (!result.ok) {
      replacing_.erase(name);
      SetStatus("Could not start the update: " + error_help::Describe(result.error), true);
      RebuildInstalled();
    }
  });
}

void RunnersPage::SetupTool(const RunnerTool& tool) {
  api::SetupRunnerToolAsync(this, tool.id, [this, label = tool.label](RunnerDownloadResult result) {
    if (!result.ok) {
      SetStatus(QString("Could not install %1: %2").arg(QString::fromStdString(label), error_help::Describe(result.error)),
                true);
      RefreshTools();
    }
  });
}

void RunnersPage::SetDefault(const std::string& reference) {
  api::PatchConfigAsync(this, {ConfigEdit{kDefaultKey, "a string", reference}},
                                [this](PatchConfigResult result) {
                                  if (!result.ok) {
                                    SetStatus("Could not set the default: " +
                                                  error_help::Describe(result.error),
                                              true);
                                    return;
                                  }
                                  RefreshInstalled();
                                });
}

void RunnersPage::Remove(const RunnerInfo& runner) {
  const QString name = QString::fromStdString(runner.label);
  if (!notify::Confirm(this, "Remove runner", QString("Remove %1? This deletes its files.").arg(name), "Remove",
                       /*destructive=*/true)) {
    return;
  }
  api::DeleteRunnerAsync(this, runner.kind, runner.name, [this](RunnerRemoveResult result) {
    if (!result.ok) {
      SetStatus("Could not remove that runner: " + error_help::Describe(result.error), true);
    }
    // Otherwise runners.removed refreshes the lists.
  });
}

void RunnersPage::Download(const std::string& tag) {
  const std::string kind = CurrentKind();
  const std::string source = source_->currentData().toString().toStdString();
  api::DownloadRunnerAsync(this, kind, tag, source, [this](RunnerDownloadResult result) {
    // The tracker hears the rest on the event stream.
    if (!result.ok) SetStatus("Could not start the download: " + error_help::Describe(result.error), true);
  });
}

void RunnersPage::DownloadChanged(const QString& key) {
  if (key.startsWith("tool:")) {
    const DownloadTracker::Entry* entry = downloads_->Find(key);
    if (entry != nullptr && entry->state == DownloadTracker::State::Failed) {
      SetStatus(QString("Installing %1 failed: %2").arg(entry->source, error_help::Describe(entry->error)), true);
    }
    if (entry != nullptr && entry->state != DownloadTracker::State::Running) {
      RefreshTools();
      RefreshInstalled();  // Proton builds need umu to be listed
    } else {
      RebuildTools();
    }
    return;
  }
  if (!key.startsWith("runner:")) return;
  const DownloadTracker::Entry* entry = downloads_->Find(key);
  if (entry == nullptr) return;
  if (entry->state == DownloadTracker::State::Failed) {
    const QString name = downloads_->NameFor(*entry);
    SetStatus(QString("Downloading %1 failed: %2").arg(name, error_help::Describe(entry->error)), true);
    replacing_.erase(entry->ref.toStdString());
  }
  if (entry->state == DownloadTracker::State::Finished) {
    // Discoverable once on disk; the list re-discovers each time.
    RefreshInstalled();
    RefreshUpdates();
    if (!catalog_loaded_ || std::ranges::any_of(releases_, [&](const RunnerRelease& release) {
          return QString::fromStdString(release.name) == entry->ref;
        })) {
      RefreshCatalog();
    }
    if (auto it = replacing_.find(entry->ref.toStdString()); it != replacing_.end()) {
      const std::string old_reference = it->second;
      replacing_.erase(it);
      const auto old = std::ranges::find(runners_, old_reference, &RunnerInfo::reference);
      // Copied: Confirm runs an event loop that can reassign runners_.
      const std::string kind = old != runners_.end() ? old->kind : std::string();
      const std::string name = old != runners_.end() ? old->name : std::string();
      if (old != runners_.end() &&
          notify::Confirm(this, "Update finished",
                          QString("Games that used %1 now use %2. Remove %1?")
                              .arg(QString::fromStdString(old->label), downloads_->NameFor(*entry)),
                          "Remove", /*destructive=*/true)) {
        api::DeleteRunnerAsync(this, kind, name, [this](RunnerRemoveResult result) {
          if (!result.ok) SetStatus("Could not remove the old build: " + error_help::Describe(result.error), true);
        });
      }
    }
  }
  RebuildInstalled();
  RebuildCatalog();
}

}  // namespace mira_gui
