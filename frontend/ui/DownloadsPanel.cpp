#include "DownloadsPanel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QVBoxLayout>

#include "ArtworkStore.h"
#include "DownloadTracker.h"
#include "Icons.h"
#include "Theme.h"

namespace mira_gui {
namespace {

using Kind = DownloadTracker::Kind;
using State = DownloadTracker::State;

const QSize kCover(40, 60);

QString RunningText(const DownloadTracker::Entry& entry) {
  switch (entry.kind) {
    case Kind::Game:
      return entry.bytes > 0 ? "Installing… " + QLocale().formattedDataSize(entry.bytes) + " written"
                             : QString("Installing…");
    case Kind::Title: {
      if (entry.source == "steam" && !entry.update) return "Handing to Steam…";
      const QString verb = entry.update ? "Updating…" : "Installing…";
      const QString progress = DownloadTracker::ProgressText(entry);
      return progress.isEmpty() ? verb : verb + " " + progress;
    }
    case Kind::Launcher: return "Installing…";
    case Kind::Job: return entry.message.isEmpty() ? QString("Working…") : entry.message;
    default: return "Downloading…";
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
  auto* list = new QWidget(scroll);
  rows_ = new QVBoxLayout(list);
  rows_->setContentsMargins(0, 0, 0, 0);
  rows_->setSpacing(0);
  scroll->setWidget(list);
  layout->addWidget(scroll);

  connect(tracker_, &DownloadTracker::Changed, this, [this](const QString& key) {
    if (isVisible() && !UpdateRow(key)) Rebuild();
  });
  connect(artwork_, &ArtworkStore::CoverChanged, this, [this] {
    if (isVisible()) Rebuild();
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

  // Grows with its rows up to a cap, then scrolls.
  const int row_height = kCover.height() + 20;
  auto* scroll = findChild<QScrollArea*>("downloads_scroll");
  scroll->setVisible(!entries.empty());
  scroll->setFixedHeight(std::min<int>(static_cast<int>(entries.size()), 6) * row_height);
  adjustSize();
}

bool DownloadsPanel::UpdateRow(const QString& key) {
  const DownloadTracker::Entry* entry = tracker_->Find(key);
  if (entry == nullptr || entry->state != State::Running) return false;
  for (int i = 0; i < rows_->count(); ++i) {
    QWidget* row = rows_->itemAt(i)->widget();
    if (row == nullptr || row->property("download_key").toString() != key) continue;
    if (!row->property("download_running").toBool()) return false;
    const QString origin = Origin(*tracker_, *entry);
    const QString state = RunningText(*entry);
    row->findChild<QLabel*>("download_title")->setText(tracker_->NameFor(*entry));
    row->findChild<QLabel*>("download_status")->setText(origin.isEmpty() ? state : origin + "  ·  " + state);
    auto* bar = row->findChild<QProgressBar*>();
    if (entry->progress >= 0) {
      bar->setRange(0, 100);
      bar->setValue(qRound(entry->progress * 100));
    } else {
      bar->setRange(0, 0);
    }
    return true;
  }
  return false;
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
  const qreal dpr = devicePixelRatioF();
  if (entry.kind == Kind::Game) {
    GameSummary game;
    game.id = entry.ref.toStdString();
    game.name = name.toStdString();
    cover->setPixmap(artwork_->Cover(game, kCover, dpr));
  } else if (entry.kind == Kind::Title) {
    cover->setPixmap(artwork_->TitleCover(entry.source, entry.ref, name, kCover, dpr));
  } else {
    icons::Glyph glyph = icons::Glyph::Download;
    if (entry.kind == Kind::Runner || entry.kind == Kind::Tool) glyph = icons::Glyph::Wrench;
    if (entry.kind == Kind::Job) {
      if (entry.source == "scan") glyph = icons::Glyph::Search;
      if (entry.source == "relocate") glyph = icons::Glyph::Refresh;
      if (entry.source == "metadata") glyph = icons::Glyph::Image;
      if (entry.source == "delete" || entry.source == "remove_source") glyph = icons::Glyph::Trash;
    }
    cover->setPixmap(icons::For(glyph, tokens.text_muted).pixmap(22, 22));
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
    if (!entry.error.isEmpty()) state += ": " + entry.error;
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
    auto* bar = new QProgressBar(row);
    if (entry.progress >= 0) {
      bar->setRange(0, 100);
      bar->setValue(qRound(entry.progress * 100));
    } else {
      bar->setRange(0, 0);
    }
    bar->setTextVisible(false);
    bar->setFixedHeight(4);
    text->addWidget(bar);
  }
  text->addStretch(1);
  layout->addLayout(text, /*stretch=*/1);

  const QString game_id = DownloadTracker::GameIdFor(entry);
  const bool tracked = tracker_->game_name && !tracker_->game_name(game_id.toStdString()).isEmpty();
  if (entry.state == State::Finished && !game_id.isEmpty() && tracked) {
    auto* show = new QPushButton("Show", row);
    connect(show, &QPushButton::clicked, this, [this, game_id] {
      hide();
      emit ShowGameRequested(game_id);
    });
    layout->addWidget(show, 0, Qt::AlignVCenter);
  }
  return row;
}

}  // namespace mira_gui
