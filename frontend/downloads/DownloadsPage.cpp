#include "DownloadsPage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "../activity/DownloadList.h"
#include "../activity/DownloadTracker.h"
#include "../app/Notify.h"
#include "../client/api/Downloads.h"
#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ListEdit.h"
#include "../widgets/PageHeader.h"
#include "../widgets/PopupDialog.h"
#include "../widgets/Scrolling.h"
#include "LinkDownloads.h"

namespace mira_gui {
namespace {

using Entry = DownloadTracker::Entry;
using State = DownloadTracker::State;

bool NeedsUser(const Entry& entry) {
  return entry.state == State::Failed || entry.link_state == "password";
}

bool Waiting(const Entry& entry) {
  return !NeedsUser(entry) && (entry.state == State::Paused || entry.link_state == "queued" ||
                               entry.link_state == "retrying");
}

bool InProgress(const Entry& entry) {
  return entry.state == State::Running && !Waiting(entry);
}

bool IsRunningLink(const DownloadInfo& download) {
  return download.state == "downloading" || download.state == "service" ||
         download.state == "queued" || download.state == "retrying";
}

}  // namespace

DownloadsPage::DownloadsPage(LinkDownloads* links, DownloadTracker* tracker, ArtworkStore* artwork,
                             QWidget* parent)
    : QWidget(parent), links_(links), tracker_(tracker) {
  setObjectName("downloads_page");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  header_ = new PageHeader("Downloads", this);

  pause_all_ = new QPushButton("Pause all", this);
  connect(pause_all_, &QPushButton::clicked, this, [this] {
    const auto& list = links_->Downloads();
    const bool running = std::ranges::any_of(list, IsRunningLink);
    for (const DownloadInfo& download : list) {
      if (running && IsRunningLink(download)) links_->Act(download.id, "pause");
      if (!running && download.state == "paused") links_->Act(download.id, "resume");
    }
  });
  header_->AddControl(pause_all_);
  auto* passwords = new QPushButton("Passwords", this);
  passwords->setToolTip("The archive passwords Mira tries on what it downloads");
  connect(passwords, &QPushButton::clicked, this, &DownloadsPage::EditPasswords);
  header_->AddControl(passwords);
  auto* settings = new QToolButton(this);
  settings->setAutoRaise(true);
  icons::Follow(settings, icons::Glyph::Settings);
  settings->setToolTip("Download settings: speed limit, how many at once");
  connect(settings, &QToolButton::clicked, this, &DownloadsPage::SettingsRequested);
  header_->AddControl(settings);
  outer->addWidget(header_);

  auto* scroll = new QScrollArea(this);
  scroll->setObjectName("settings_page");
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  SetUpScrolling(scroll, this);
  auto* body = new QWidget();
  body->setObjectName("settings_canvas");
  auto* body_layout = new QVBoxLayout(body);
  body_layout->setContentsMargins(32, 12, 32, 22);
  body_layout->setSpacing(16);
  empty_ = MakeLabel(body,
                     "Nothing is downloading or installing. Add games from links with Add "
                     "games › Download from links.",
                     "subtle");
  empty_->setWordWrap(true);
  body_layout->addWidget(empty_);

  const auto add_section = [&](const QString& heading,
                               std::function<bool(const Entry&)> filter) -> Section& {
    Section section;
    section.card = new SettingsCard(heading, body);
    section.list = new DownloadList(tracker_, artwork, DownloadList::Style::Full, section.card);
    section.list->SetFilter(std::move(filter));
    section.card->AddRow(section.list);
    body_layout->addWidget(section.card);
    connect(section.list, &DownloadList::Rebuilt, this, &DownloadsPage::Refresh);
    connect(section.list, &DownloadList::ShowGameRequested, this,
            &DownloadsPage::ShowGameRequested);
    connect(section.list, &DownloadList::LogRequested, this, &DownloadsPage::LogRequested);
    // A row's fix opens something else (Settings, a runner): the page makes way for it.
    connect(section.list, &DownloadList::Leaving, this, &DownloadsPage::CloseRequested);
    connect(section.list, &DownloadList::LinkAction, this,
            [this](const QString& id, const QString& action) {
              links_->Act(id.toStdString(), action.toStdString());
            });
    connect(section.list, &DownloadList::LinkPasswordEntered, this,
            [this](const QString& id, const QString& password) {
              links_->Unskip(id.toStdString());
              links_->SubmitPassword(id.toStdString(), password.toStdString());
            });
    connect(section.list, &DownloadList::LinkRemoveRequested, this, [this](const QString& id) {
      const DownloadInfo* download = links_->Find(id.toStdString());
      if (download != nullptr && download->state != "done" &&
          !notify::Confirm(this, "Cancel this download?", "What it downloaded so far is deleted",
                           "Cancel download"))
        return;
      links_->Remove(id.toStdString());
    });
    sections_.push_back(section);
    return sections_.back();
  };
  add_section("Needs you", NeedsUser);
  add_section("Downloading and installing", InProgress);
  // In the order they'll start, so Move ahead shows.
  DownloadList* waiting = add_section("Waiting", Waiting).list;
  waiting->SetOrder([this](const Entry& entry) {
    const auto& list = links_->Downloads();
    const auto found = std::ranges::find(list, entry.ref.toStdString(), &DownloadInfo::id);
    return entry.kind == DownloadTracker::Kind::Link && found != list.end()
               ? static_cast<int>(found - list.begin())
               : static_cast<int>(list.size());
  });
  Section& finished =
      add_section("Finished", [](const Entry& entry) { return entry.state == State::Finished; });
  auto* clear = new QPushButton("Clear", finished.card);
  clear->setObjectName("text_button");
  connect(clear, &QPushButton::clicked, this, [this] {
    tracker_->ClearFinished();
    links_->ClearFinished();
  });
  finished.card->Header()->addWidget(clear);
  body_layout->addStretch(1);
  scroll->setWidget(body);
  outer->addWidget(scroll, /*stretch=*/1);

  connect(tracker_, &DownloadTracker::Changed, this, &DownloadsPage::UpdateHeader);
  // The whole list read again: Waiting's order may have changed. Only it, so a password being
  // typed in Needs you keeps its box.
  connect(links_, &LinkDownloads::Changed, waiting, [waiting](const QString& id) {
    if (id.isEmpty()) waiting->Rebuild();
  });
  links_->SeenFailures();
  for (Section& section : sections_) section.list->Rebuild();
}

void DownloadsPage::Refresh() {
  bool any = false;
  for (const Section& section : sections_) {
    section.card->setVisible(section.list->Count() > 0);
    any = any || section.list->Count() > 0;
  }
  empty_->setVisible(!any);
  UpdateHeader();
}

void DownloadsPage::UpdateHeader() {
  double speed = 0;
  int running = 0, waiting = 0, needs_you = 0;
  for (const Entry& entry : tracker_->Entries()) {
    if (NeedsUser(entry)) ++needs_you;
    if (Waiting(entry)) ++waiting;
    if (!InProgress(entry)) continue;
    ++running;
    speed += std::max(0.0, entry.bytes_per_second);
  }
  QStringList parts;
  if (needs_you > 0) parts << QString("%1 need you").arg(needs_you);
  if (running > 0) parts << QString("%1 running").arg(running);
  if (waiting > 0) parts << QString("%1 waiting").arg(waiting);
  if (speed > 0) parts << "↓ " + SizeText(static_cast<qint64>(speed)) + "/s";
  header_->SetSubtitle(parts.isEmpty() ? QString("Nothing running") : parts.join("  ·  "));
  const auto& list = links_->Downloads();
  const bool links_running = std::ranges::any_of(list, IsRunningLink);
  const bool links_paused = std::ranges::any_of(
      list, [](const DownloadInfo& download) { return download.state == "paused"; });
  pause_all_->setText(links_running || !links_paused ? "Pause all" : "Resume all");
  pause_all_->setVisible(links_running || links_paused);
}

void DownloadsPage::EditPasswords() {
  api::GetDownloadPasswordsAsync(this, [this](DownloadPasswordsResult result) {
    if (!result.ok)
      return notify::FailedRequest(this, "Could not read the saved passwords", result.error);
    PopupDialog dialog(this, Tone::Info, "Saved passwords");
    dialog.SetMessage("Tried on every encrypted archive, most recently working first.");
    dialog.SetWidth(460);
    auto* list = new ListEdit(ListEdit::Kind::Text, "Add password", &dialog);
    QStringList items;
    for (const std::string& password : result.passwords) items << QString::fromStdString(password);
    list->SetItems(items);
    dialog.AddWidget(list);
    dialog.AddButton("Cancel", /*accept=*/false);
    dialog.AddButton("Save", /*accept=*/true, /*default_button=*/true);
    if (dialog.exec() != QDialog::Accepted) return;
    std::vector<std::string> edited;
    for (const QString& item : list->Items()) edited.push_back(item.toStdString());
    api::SetDownloadPasswordsAsync(this, edited, [this](StoreActionResult saved) {
      if (!saved.ok) notify::FailedRequest(this, "Could not save the passwords", saved.error);
    });
  });
}

}  // namespace mira_gui
