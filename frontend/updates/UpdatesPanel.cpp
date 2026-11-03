#include "UpdatesPanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <json.hpp>

#include "../activity/DownloadTracker.h"
#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../client/api/Emulators.h"
#include "../client/api/ModManagers.h"
#include "../client/api/Runners.h"
#include "../client/api/Stores.h"
#include "../runners/RunnerUpdater.h"
#include "../system/Distro.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ProgressRail.h"
#include "../widgets/Scrolling.h"
#include "MiraUpdater.h"
#include "UpdateChecker.h"
#include "UpdateRows.h"

namespace mira_gui {
namespace {

constexpr int kMaxListHeight = 460;

std::string StartedKey(const UpdateRow& update) {
  return update.kind + ":" + update.id;
}

// "proton:GE-Proton10-25" -> {"proton", "GE-Proton10-25"}.
std::pair<std::string, std::string> SplitReference(const std::string& reference) {
  const auto colon = reference.find(':');
  if (colon == std::string::npos) return {std::string(), reference};
  return {reference.substr(0, colon), reference.substr(colon + 1)};
}

QFrame* NewRow(bool first) {
  auto* row = new QFrame();
  row->setObjectName("download_row");
  row->setProperty("first", first);
  return row;
}

QLabel* ReleaseLink(QWidget* parent, const UpdateInfo& mira) {
  auto* label = new QLabel(
      QString("%1 → %2  ·  <a href=\"%3\" style=\"color: %4; text-decoration: none;\">What's "
              "new</a>")
          .arg(QString::fromStdString(mira.installed), QString::fromStdString(mira.latest),
               QString::fromStdString(mira.url), theme::Current().accent.name()),
      parent);
  label->setProperty("role", "muted");
  label->setTextFormat(Qt::RichText);
  label->setOpenExternalLinks(true);
  return label;
}

}  // namespace

UpdatesPanel::UpdatesPanel(UpdateChecker* checker, MiraUpdater* mira, RunnerUpdater* runners,
                           DownloadTracker* downloads, QWidget* parent)
    : QWidget(parent, Qt::Popup),
      checker_(checker),
      mira_(mira),
      runners_(runners),
      downloads_(downloads) {
  setObjectName("updates_popover");
  setAttribute(Qt::WA_StyledBackground);
  setFixedWidth(380);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 6);
  layout->setSpacing(0);

  auto* header = new QHBoxLayout();
  header->setContentsMargins(18, 12, 10, 6);
  auto* title = new QLabel("Updates", this);
  title->setProperty("role", "section");
  header->addWidget(title);
  header->addStretch(1);
  auto* check = new QPushButton("Check again", this);
  check->setObjectName("text_button");
  check->setProperty("tone", "muted");
  connect(check, &QPushButton::clicked, this, [this] { checker_->Check(/*refresh=*/true); });
  header->addWidget(check);
  update_all_ = new QPushButton("Update all", this);
  update_all_->setObjectName("text_button");
  connect(update_all_, &QPushButton::clicked, this, &UpdatesPanel::UpdateAll);
  header->addWidget(update_all_);
  layout->addLayout(header);

  empty_ = new QLabel("Mira and everything it downloaded are up to date.", this);
  empty_->setProperty("role", "subtle");
  empty_->setWordWrap(true);
  empty_->setContentsMargins(18, 6, 18, 10);
  layout->addWidget(empty_);

  auto* scroll = new QScrollArea(this);
  scroll->setObjectName("updates_scroll");
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  SetUpScrolling(scroll, this);
  auto* list = new QWidget(scroll);
  rows_ = new QVBoxLayout(list);
  rows_->setContentsMargins(0, 0, 0, 0);
  rows_->setSpacing(0);
  scroll->setWidget(list);
  layout->addWidget(scroll);

  const auto rebuild = [this] {
    if (isVisible()) Rebuild();
  };
  connect(checker_, &UpdateChecker::Changed, this, rebuild);
  connect(mira_, &MiraUpdater::Changed, this, rebuild);
  connect(downloads_, &DownloadTracker::Changed, this, [this, rebuild](const QString& key) {
    if (key.startsWith("runner:")) rebuild();
  });
  connect(EventHub::Instance(), &EventHub::Received, this,
          [this, rebuild](const std::string& type, const std::string& data, bool) {
            if (type != "job.started" && type != "job.finished" && type != "job.failed") return;
            const auto job = nlohmann::json::parse(data, nullptr, false);
            if (!job.is_object()) return;
            const std::string kind = job.value("kind", std::string());
            if (kind != "install" && kind != "setup") return;
            const std::string target = job.value("target", std::string());
            if (type == "job.started") {
              running_.insert(target);
            } else {
              running_.erase(target);
            }
            rebuild();
          });
  connect(mira_, &MiraUpdater::Progress, this, [this] {
    if (mira_status_ != nullptr && !mira_->Message().isEmpty())
      mira_status_->setText("Downloading  ·  " + mira_->Message());
    if (mira_rail_ != nullptr) mira_rail_->SetProgress(mira_->Percent() / 100.0);
  });
}

void UpdatesPanel::ShowBelow(QWidget* anchor) {
  anchor_ = anchor;
  Rebuild();
  const QPoint corner = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 4));
  move(corner.x() - width(), corner.y());
  show();
}

void UpdatesPanel::StartMira(QWidget* anchor) {
  const auto entry = MiraEntry(checker_->Updates());
  const bool can_update =
      entry && (entry->install == "appimage" || entry->install == "deb" || entry->install == "rpm");
  anchor_ = anchor;
  if (can_update) {
    StartMiraUpdate(QString::fromStdString(entry->latest));
  } else {
    ShowBelow(anchor);
  }
}

void UpdatesPanel::StartMiraUpdate(const QString& latest) {
  // A popup would take the confirm prompt's clicks.
  hide();
  mira_->Start(latest);
  if (mira_->Busy() && anchor_ != nullptr) ShowBelow(anchor_);
}

int UpdatesPanel::Pending() const {
  int count = MiraEntry(checker_->Updates()) ? 1 : 0;
  for (const UpdateGroup& group : GroupUpdates(checker_->Updates()))
    count += static_cast<int>(group.rows.size());
  return count;
}

void UpdatesPanel::Rebuild() {
  mira_status_ = nullptr;
  mira_rail_ = nullptr;
  while (QLayoutItem* item = rows_->takeAt(0)) {
    // Deferred: a row's own button click may be what led here.
    if (item->widget() != nullptr) {
      item->widget()->hide();
      item->widget()->deleteLater();
    }
    delete item;
  }
  bool first = true;
  if (MiraEntry(checker_->Updates())) {
    rows_->addWidget(BuildMiraRow());
    first = false;
  }
  bool idle_row = false;
  for (const UpdateGroup& group : GroupUpdates(checker_->Updates())) {
    auto* heading = NewRow(first);
    auto* heading_layout = new QVBoxLayout(heading);
    heading_layout->setContentsMargins(18, 10, 18, 2);
    heading_layout->addWidget(MakeGroupHeading(heading, group.heading));
    rows_->addWidget(heading);
    first = false;
    bool first_in_group = true;
    for (const UpdateRow& row : group.rows) {
      rows_->addWidget(BuildRow(row, first_in_group));
      first_in_group = false;
      idle_row = idle_row || !Updating(row);
    }
  }
  rows_->addStretch(1);
  update_all_->setVisible(idle_row);
  // A row added to a visible list stays hidden until later, and would count for no height below.
  for (int i = 0; i < rows_->count(); ++i) {
    if (QWidget* row = rows_->itemAt(i)->widget()) row->show();
  }

  const bool any = rows_->count() > 1;
  empty_->setVisible(!any);
  auto* scroll = findChild<QScrollArea*>("updates_scroll");
  scroll->setVisible(any);
  const int height = rows_->hasHeightForWidth() ? rows_->totalHeightForWidth(width())
                                                : rows_->totalSizeHint().height();
  scroll->setFixedHeight(std::min(height, kMaxListHeight));
  adjustSize();
}

QWidget* UpdatesPanel::BuildMiraRow() {
  const UpdateInfo entry = *MiraEntry(checker_->Updates());
  auto* row = NewRow(/*first=*/true);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(18, 10, 18, 10);
  layout->setSpacing(12);

  auto* badge = new QLabel("M", row);
  badge->setObjectName("brand_badge");
  badge->setFixedSize(24, 24);
  badge->setAlignment(Qt::AlignCenter);
  auto* badge_box = new QVBoxLayout();
  badge_box->setContentsMargins(8, 0, 8, 0);
  badge_box->addWidget(badge);
  layout->addLayout(badge_box);

  auto* text = new QVBoxLayout();
  text->setSpacing(3);
  auto* title = new QLabel("Mira", row);
  title->setStyleSheet("font-weight: 600;");
  text->addWidget(title);

  const auto line = [row, text](const QString& message, const char* role = "muted") {
    auto* label = MakeLabel(row, message, role);
    text->addWidget(label);
    return label;
  };
  QPushButton* action = nullptr;
  switch (mira_->Current()) {
    case MiraUpdater::State::Downloading:
      mira_status_ = line(mira_->Message().isEmpty() ? QString("Downloading")
                                                     : "Downloading  ·  " + mira_->Message());
      mira_rail_ = new ProgressRail(row);
      mira_rail_->SetProgress(mira_->Message().isEmpty() ? -1 : mira_->Percent() / 100.0);
      text->addWidget(mira_rail_);
      action = new QPushButton("Cancel", row);
      connect(action, &QPushButton::clicked, mira_, &MiraUpdater::Cancel);
      break;
    case MiraUpdater::State::Installing:
      line("Waiting for your password");
      break;
    case MiraUpdater::State::Closing:
      line("Installed  ·  Closing your games…");
      break;
    case MiraUpdater::State::Restarting:
      line("Restarting Mira…");
      break;
    case MiraUpdater::State::Failed:
      line("Failed: " + mira_->Error(), "error");
      [[fallthrough]];
    case MiraUpdater::State::Idle: {
      if (mira_->Current() == MiraUpdater::State::Idle) text->addWidget(ReleaseLink(row, entry));
      if (entry.install == "manual") {
        line(system::DetectDistro().family == system::Distro::Family::Arch
                 ? QString("Built from the PKGBUILD, so rebuild it to update.")
                 : QString("Mira can't replace this install, so get the new version from its "
                           "release page."));
        break;
      }
      const bool failed = mira_->Current() == MiraUpdater::State::Failed;
      if (!failed) {
        line(entry.install == "appimage" ? QString("Restarts Mira when it's done.")
                                         : QString("Asks for your password, then restarts Mira."));
      }
      action = new QPushButton(failed ? "Retry" : "Update", row);
      action->setDefault(!failed);
      action->setToolTip("Update Mira to " + QString::fromStdString(entry.latest));
      connect(action, &QPushButton::clicked, this,
              [this, latest = QString::fromStdString(entry.latest)] { StartMiraUpdate(latest); });
      break;
    }
  }
  layout->addLayout(text, /*stretch=*/1);
  if (action != nullptr) layout->addWidget(action, 0, Qt::AlignVCenter);
  return row;
}

bool UpdatesPanel::Updating(const UpdateRow& update) const {
  if (started_.contains(StartedKey(update))) return true;
  // These all run on after their request is answered.
  if (update.kind != "runner") return running_.contains(update.id);
  const auto [kind, name] = SplitReference(update.id);
  const auto* entry = downloads_->Find(
      DownloadTracker::KeyFor(DownloadTracker::Kind::Runner, QString::fromStdString(kind),
                              QString::fromStdString(update.release)));
  return entry != nullptr && entry->state == DownloadTracker::State::Running;
}

QWidget* UpdatesPanel::BuildRow(const UpdateRow& update, bool first) {
  auto* row = NewRow(first);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(18, 8, 18, 8);
  layout->setSpacing(12);

  auto* icon = new QLabel(row);
  icon->setFixedWidth(40);
  icon->setAlignment(Qt::AlignCenter);
  icons::Follow(icon, update.kind == "emulator" ? icons::Glyph::Gamepad : icons::Glyph::Wrench, 22);
  layout->addWidget(icon);

  auto* text = new QVBoxLayout();
  text->setSpacing(3);
  auto* title = new ElidedLabel(update.name, row);
  title->setStyleSheet("font-weight: 600;");
  title->setToolTip(update.name);
  text->addWidget(title);
  const bool updating = Updating(update);
  text->addWidget(MakeLabel(row, updating ? QString("Updating…") : update.detail, "muted"));
  layout->addLayout(text, /*stretch=*/1);

  if (!updating) {
    auto* button = new QPushButton("Update", row);
    button->setDefault(true);
    button->setToolTip("Update " + update.name);
    connect(button, &QPushButton::clicked, this, [this, update] { Update(update); });
    layout->addWidget(button, 0, Qt::AlignVCenter);
  }
  return row;
}

void UpdatesPanel::Update(const UpdateRow& update) {
  started_.insert(StartedKey(update));
  const auto done = [this, key = StartedKey(update), name = update.name](bool ok,
                                                                         const ApiError& error) {
    started_.erase(key);
    if (!ok) notify::FailedRequest(this, "Could not update " + name + ".", error);
    if (isVisible()) Rebuild();
  };
  if (update.kind == "runner") {
    const auto [kind, name] = SplitReference(update.id);
    runners_->Update(
        {.reference = update.id, .kind = kind, .name = name, .label = update.installed},
        update.release, done);
  } else if (update.kind == "emulator") {
    api::InstallEmulatorAsync(this, update.id,
                              [done](StoreActionResult r) { done(r.ok, r.error); });
  } else if (update.kind == "mod_manager") {
    api::InstallModManagerAsync(this, update.id,
                                [done](StoreActionResult r) { done(r.ok, r.error); });
  } else if (update.id == "umu" || update.id == "winetricks") {
    api::SetupRunnerToolAsync(this, update.id,
                              [done](RunnerDownloadResult r) { done(r.ok, r.error); });
  } else {
    api::SetupStoreToolAsync(this, update.id, [done](StoreActionResult r) { done(r.ok, r.error); });
  }
  if (isVisible()) Rebuild();
}

void UpdatesPanel::UpdateAll() {
  for (const UpdateGroup& group : GroupUpdates(checker_->Updates())) {
    for (const UpdateRow& row : group.rows) {
      if (!Updating(row)) Update(row);
    }
  }
}

}  // namespace mira_gui
