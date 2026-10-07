#include "DownloadsPanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QVBoxLayout>

#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/Jobs.h"
#include "../library/ArtworkStore.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ProgressRail.h"
#include "../widgets/Scrolling.h"
#include "DownloadTracker.h"

namespace mira_gui {
namespace {

using Kind = DownloadTracker::Kind;
using State = DownloadTracker::State;

const QSize kCover(40, 60);

QString RunningText(const DownloadTracker::Entry& entry) {
  switch (entry.kind) {
    case Kind::Game:
      return entry.bytes > 0 ? "Installing… " + SizeText(entry.bytes) + " written"
                             : QString("Installing…");
    case Kind::Title: {
      if (entry.source == "steam" && !entry.update) return "Handing to Steam…";
      const QString verb = entry.update ? "Updating…" : "Installing…";
      const QString progress = DownloadTracker::ProgressText(entry);
      return progress.isEmpty() ? verb : verb + " " + progress;
    }
    case Kind::Launcher: return "Installing…";
    case Kind::Job: return entry.message.isEmpty() ? QString("Working…") : entry.message;
    default: {
      const QString progress = DownloadTracker::ProgressText(entry);
      return progress.isEmpty() ? QString("Downloading…") : "Downloading… " + progress;
    }
  }
}

QString FinishedText(const DownloadTracker::Entry& entry) {
  switch (entry.kind) {
    case Kind::Game:
    case Kind::Launcher: return "Installed";
    case Kind::Title:
      if (entry.update) return "Updated";
      return entry.source == "steam" ? "Sent to Steam" : "Installed";
    case Kind::Job: return "Done";
    default: return "Downloaded";
  }
}

// What it's from, before the state: "GOG", "Proton", or nothing for a
// game's own installer.
QString Origin(const DownloadTracker& tracker, const DownloadTracker::Entry& entry) {
  switch (entry.kind) {
    case Kind::Game:
    case Kind::Job: return QString();
    case Kind::Runner: return entry.source.isEmpty() ? QString() : entry.source.left(1).toUpper() + entry.source.mid(1);
    case Kind::Tool: return tracker.source_name ? tracker.source_name(entry.source) + " helper" : QString();
    default: return tracker.source_name ? tracker.source_name(entry.source) : entry.source;
  }
}

}  // namespace

DownloadsPanel::DownloadsPanel(DownloadTracker* tracker, ArtworkStore* artwork, QWidget* parent)
    : QWidget(parent, Qt::Popup), tracker_(tracker), artwork_(artwork) {
  setObjectName("downloads_popover");
  setAttribute(Qt::WA_StyledBackground);
  setFixedWidth(380);
  // Laid out as a settings card: titled header, then rows split by a line.
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 6);
  layout->setSpacing(0);

  auto* header = new QHBoxLayout();
  header->setContentsMargins(18, 12, 10, 6);
  auto* title = new QLabel("Activity", this);
  title->setProperty("role", "section");
  header->addWidget(title);
  header->addStretch(1);
  clear_ = new QPushButton("Clear finished", this);
  clear_->setObjectName("text_button");
  connect(clear_, &QPushButton::clicked, tracker_, &DownloadTracker::ClearFinished);
  header->addWidget(clear_);
  layout->addLayout(header);

  empty_ = new QLabel("Nothing installing, downloading or running.", this);
  empty_->setProperty("role", "subtle");
  empty_->setContentsMargins(18, 6, 18, 10);
  layout->addWidget(empty_);

  auto* scroll = new QScrollArea(this);
  scroll->setObjectName("downloads_scroll");
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

  connect(tracker_, &DownloadTracker::Changed, this, [this](const QString& key) {
    if (isVisible() && !UpdateRow(key)) Rebuild();
  });
  // Only the rows showing that cover: a library-wide cover load would otherwise rebuild every row
  // per cover.
  connect(artwork_, &ArtworkStore::CoverChanged, this, [this](const QString& id) {
    if (isVisible()) UpdateCover(id);
  });
}

void DownloadsPanel::ShowBelow(QWidget* anchor) {
  Rebuild();
  const QPoint corner = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 4));
  move(corner.x() - width(), corner.y());
  show();
}

void DownloadsPanel::Rebuild() {
  while (QLayoutItem* item = rows_->takeAt(0)) {
    // Deferred: a row's own button click may be what led here.
    if (item->widget() != nullptr) {
      item->widget()->hide();
      item->widget()->deleteLater();
    }
    delete item;
  }
  const auto& entries = tracker_->Entries();
  for (int i = 0; i < static_cast<int>(entries.size()); ++i) rows_->addWidget(BuildRow(i));
  rows_->addStretch(1);
  empty_->setVisible(entries.empty());
  clear_->setEnabled(tracker_->RunningCount() < static_cast<int>(entries.size()));

  // Grows with its rows up to six, then scrolls. Measured at the popover's width, since a
  // failed row's message and hint wrap onto more lines than a cover's height.
  int height = 0;
  for (int i = 0; i < std::min(rows_->count(), 6); ++i) {
    if (QWidget* row = rows_->itemAt(i)->widget()) {
      height += row->hasHeightForWidth() ? row->heightForWidth(width()) : row->sizeHint().height();
    }
  }
  auto* scroll = findChild<QScrollArea*>("downloads_scroll");
  scroll->setVisible(!entries.empty());
  scroll->setFixedHeight(height);
  adjustSize();
}

bool DownloadsPanel::UpdateRow(const QString& key) {
  const DownloadTracker::Entry* entry = tracker_->Find(key);
  if (entry == nullptr || entry->state != State::Running) return false;
  for (int i = 0; i < rows_->count(); ++i) {
    QWidget* row = rows_->itemAt(i)->widget();
    if (row == nullptr || row->property("download_key").toString() != key) continue;
    if (!row->property("download_running").toBool()) return false;
    // Its job became known after the row was built: rebuilt with Cancel.
    if ((row->findChild<QPushButton*>("download_cancel") == nullptr) != tracker_->JobFor(*entry).isEmpty()) return false;
    const QString origin = Origin(*tracker_, *entry);
    const QString state = RunningText(*entry);
    row->findChild<QLabel*>("download_title")->setText(tracker_->NameFor(*entry));
    row->findChild<QLabel*>("download_status")->setText(origin.isEmpty() ? state : origin + "  ·  " + state);
    row->findChild<ProgressRail*>()->SetProgress(entry->progress);
    return true;
  }
  return false;
}

namespace {

// A game's or store title's row cover.
QPixmap EntryCover(const DownloadTracker& tracker, ArtworkStore* artwork,
                   const DownloadTracker::Entry& entry, qreal dpr) {
  const QString name = tracker.NameFor(entry);
  if (entry.kind == Kind::Title)
    return artwork->TitleCover(entry.source, entry.ref, name, kCover, dpr);
  GameSummary game;
  game.id = entry.ref.toStdString();
  game.name = name.toStdString();
  return artwork->Cover(game, kCover, dpr);
}

}  // namespace

void DownloadsPanel::UpdateCover(const QString& id) {
  for (QLabel* cover : findChildren<QLabel*>("download_cover")) {
    if (cover->property("cover_id").toString() != id) continue;
    const QString key = cover->parentWidget()->property("download_key").toString();
    if (const DownloadTracker::Entry* entry = tracker_->Find(key)) {
      cover->setPixmap(EntryCover(*tracker_, artwork_, *entry, devicePixelRatioF()));
    }
  }
}

QWidget* DownloadsPanel::BuildRow(int index) {
  const DownloadTracker::Entry& entry = tracker_->Entries()[static_cast<size_t>(index)];
  const theme::Tokens& tokens = theme::Current();
  const QString name = tracker_->NameFor(entry);

  auto* row = new QFrame();
  row->setObjectName("download_row");
  row->setProperty("download_key", entry.key);
  row->setProperty("download_running", entry.state == State::Running);
  row->setProperty("first", index == 0);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(18, 10, 18, 10);
  layout->setSpacing(12);

  auto* cover = new QLabel(row);
  cover->setFixedSize(kCover);
  cover->setAlignment(Qt::AlignCenter);
  if (entry.kind == Kind::Game || entry.kind == Kind::Title) {
    cover->setObjectName("download_cover");
    cover->setProperty("cover_id",
                       entry.kind == Kind::Game ? entry.ref : entry.source + "-" + entry.ref);
    cover->setPixmap(EntryCover(*tracker_, artwork_, entry, devicePixelRatioF()));
  } else {
    icons::Glyph glyph = icons::Glyph::Download;
    if (entry.kind == Kind::Runner || entry.kind == Kind::Tool) glyph = icons::Glyph::Wrench;
    if (entry.kind == Kind::Job) {
      if (entry.source == "scan") glyph = icons::Glyph::Search;
      if (entry.source == "relocate") glyph = icons::Glyph::Refresh;
      if (entry.source == "metadata") glyph = icons::Glyph::Image;
      if (entry.source == "delete" || entry.source == "remove_source") glyph = icons::Glyph::Trash;
    }
    icons::Follow(cover, glyph, 22);
  }
  layout->addWidget(cover);

  auto* text = new QVBoxLayout();
  text->setSpacing(3);
  auto* title = new QLabel(name, row);
  title->setObjectName("download_title");
  title->setStyleSheet("font-weight: 600;");
  title->setToolTip(name);
  text->addWidget(title);

  QString state;
  const char* role = "muted";
  if (entry.state == State::Running) {
    state = RunningText(entry);
  } else if (entry.state == State::Finished) {
    state = FinishedText(entry);
  } else {
    state = "Failed";
    if (!entry.error.empty()) state += ": " + error_help::Describe(entry.error);
    role = "error";
  }
  const QString origin = Origin(*tracker_, entry);
  auto* status = new QLabel(origin.isEmpty() ? state : origin + "  ·  " + state, row);
  status->setObjectName("download_status");
  status->setProperty("role", role);
  status->setWordWrap(true);
  text->addWidget(status);

  if (entry.state == State::Running) {
    // Busy unless the source reports how far along it is.
    auto* rail = new ProgressRail(row);
    rail->SetProgress(entry.progress);
    text->addWidget(rail);
  }
  text->addStretch(1);
  layout->addLayout(text, /*stretch=*/1);

  // Every row, whatever its state: what it did, or is doing.
  auto* log = new QPushButton("Log", row);
  log->setObjectName("text_button");
  log->setToolTip("Open its live log in a window");
  connect(log, &QPushButton::clicked, this, [this, channel = DownloadTracker::LogChannelFor(entry), name] {
    emit LogRequested(channel, name);
  });
  layout->addWidget(log, 0, Qt::AlignVCenter);

  const QString game_id = DownloadTracker::GameIdFor(entry);
  const bool tracked = tracker_->game_name && !tracker_->game_name(game_id.toStdString()).isEmpty();
  if (const QString job = tracker_->JobFor(entry); !job.isEmpty()) {
    auto* cancel = new QPushButton("Cancel", row);
    cancel->setObjectName("download_cancel");
    connect(cancel, &QPushButton::clicked, this, [this, cancel, job] {
      cancel->setEnabled(false);  // the row goes once mirad says it's cancelled
      jobs::Cancel(this, job.toStdString(), [this](ApiError error) {
        if (!error.message.empty()) notify::FailedRequest(this, "Could not cancel it.", error);
      });
    });
    layout->addWidget(cancel, 0, Qt::AlignVCenter);
  } else if (entry.state == State::Finished && !game_id.isEmpty() && tracked) {
    auto* show = new QPushButton("Show", row);
    connect(show, &QPushButton::clicked, this, [this, game_id] {
      hide();
      emit ShowGameRequested(game_id);
    });
    layout->addWidget(show, 0, Qt::AlignVCenter);
  } else if (entry.state == State::Failed) {
    if (const auto action = error_help::ActionFor(entry.error)) {
      auto* fix = new QPushButton(action->label, row);
      connect(fix, &QPushButton::clicked, this, [this, run = action->run] {
        hide();
        run();
      });
      layout->addWidget(fix, 0, Qt::AlignVCenter);
    }
  }
  return row;
}

}  // namespace mira_gui
