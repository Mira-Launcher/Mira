#include "DownloadList.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/Jobs.h"
#include "../client/api/Stores.h"
#include "../library/ArtworkStore.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ProgressRail.h"

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
    case Kind::Launcher:
      return "Installing…";
    case Kind::Job:
      return entry.message.isEmpty() ? QString("Working…") : entry.message;
    case Kind::Link:
      if (!entry.message.isEmpty()) return entry.message;
      return entry.progress >= 0 ? DownloadTracker::ProgressText(entry) : QString("Downloading…");
    default: {
      const QString progress = DownloadTracker::ProgressText(entry);
      return progress.isEmpty() ? QString("Downloading…") : "Downloading… " + progress;
    }
  }
}

QString FinishedText(const DownloadTracker::Entry& entry) {
  switch (entry.kind) {
    case Kind::Game:
    case Kind::Launcher:
      return "Installed";
    case Kind::Title:
      if (entry.update) return "Updated";
      return entry.source == "steam" ? "Sent to Steam" : "Installed";
    case Kind::Job:
      return "Done";
    default:
      return "Downloaded";
  }
}

// What it's from, before the state: "GOG", "Proton", or nothing for a
// game's own installer.
QString Origin(const DownloadTracker& tracker, const DownloadTracker::Entry& entry) {
  switch (entry.kind) {
    case Kind::Game:
    case Kind::Job:
    case Kind::Link:
      return QString();
    case Kind::Runner:
      return entry.source.isEmpty() ? QString()
                                    : entry.source.left(1).toUpper() + entry.source.mid(1);
    case Kind::Tool:
      return tracker.source_name ? tracker.source_name(entry.source) + " helper" : QString();
    default:
      return tracker.source_name ? tracker.source_name(entry.source) : entry.source;
  }
}

}  // namespace

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

DownloadList::DownloadList(DownloadTracker* tracker, ArtworkStore* artwork, Style style,
                           QWidget* parent)
    : QWidget(parent), tracker_(tracker), artwork_(artwork), style_(style) {
  rows_ = new QVBoxLayout(this);
  rows_->setContentsMargins(0, 0, 0, 0);
  rows_->setSpacing(0);
  // The page's lists live only while it's open, and an empty one is hidden, so it listens anyway.
  connect(tracker_, &DownloadTracker::Changed, this, [this](const QString& key) {
    if ((isVisible() || style_ == Style::Full) && !UpdateRow(key)) Rebuild();
  });
  // Only the rows showing that cover: a library-wide cover load would otherwise rebuild every row
  // per cover.
  connect(artwork_, &ArtworkStore::CoverChanged, this, [this](const QString& id) {
    if (isVisible()) UpdateCover(id);
  });
  // A store title's cover asked for here and dropped as scrolled past.
  connect(artwork_, &ArtworkStore::RequestsDropped, this, [this] {
    if (isVisible()) Rebuild();
  });
}

void DownloadList::SetFilter(std::function<bool(const DownloadTracker::Entry&)> filter) {
  filter_ = std::move(filter);
}

void DownloadList::SetOrder(std::function<int(const DownloadTracker::Entry&)> rank) {
  rank_ = std::move(rank);
}

void DownloadList::Rebuild() {
  while (QLayoutItem* item = rows_->takeAt(0)) {
    // Deferred: a row's own button click may be what led here.
    if (item->widget() != nullptr) {
      item->widget()->hide();
      item->widget()->deleteLater();
    }
    delete item;
  }
  count_ = 0;
  std::vector<const DownloadTracker::Entry*> shown;
  for (const DownloadTracker::Entry& entry : tracker_->Entries()) {
    if (!filter_ || filter_(entry)) shown.push_back(&entry);
  }
  if (rank_)
    std::ranges::stable_sort(shown, {}, [this](const auto* entry) { return rank_(*entry); });
  for (const DownloadTracker::Entry* entry : shown) {
    rows_->addWidget(BuildRow(*entry, count_ == 0));
    ++count_;
  }
  emit Rebuilt();
}

int DownloadList::HeightFor(int width, int rows) const {
  int height = 0;
  for (int i = 0; i < std::min(rows_->count(), rows); ++i) {
    if (QWidget* row = rows_->itemAt(i)->widget()) {
      height += row->hasHeightForWidth() ? row->heightForWidth(width) : row->sizeHint().height();
    }
  }
  return height;
}

bool DownloadList::UpdateRow(const QString& key) {
  const DownloadTracker::Entry* entry = tracker_->Find(key);
  const auto shown = [this, &key] {
    for (int i = 0; i < rows_->count(); ++i) {
      QWidget* row = rows_->itemAt(i)->widget();
      if (row != nullptr && row->property("download_key").toString() == key) return true;
    }
    return false;
  };
  // Neither here nor coming here: another section's.
  if (filter_ && (entry == nullptr || !filter_(*entry)) && !shown()) return true;
  if (entry == nullptr || entry->state != State::Running) return false;
  for (int i = 0; i < rows_->count(); ++i) {
    QWidget* row = rows_->itemAt(i)->widget();
    if (row == nullptr || row->property("download_key").toString() != key) continue;
    if (!row->property("download_running").toBool()) return false;
    // A link's buttons follow its state.
    if (row->property("link_state").toString() != entry->link_state) return false;
    // Its job became known after the row was built: rebuilt with Cancel.
    if ((row->findChild<QPushButton*>("download_cancel") == nullptr) !=
        tracker_->JobFor(*entry).isEmpty())
      return false;
    const QString origin = Origin(*tracker_, *entry);
    const QString state = RunningText(*entry);
    row->findChild<QLabel*>("download_title")->setText(tracker_->NameFor(*entry));
    row->findChild<QLabel*>("download_status")
        ->setText(origin.isEmpty() ? state : origin + "  ·  " + state);
    if (auto* rail = row->findChild<ProgressRail*>()) rail->SetProgress(entry->progress);
    return true;
  }
  return false;
}

void DownloadList::UpdateCover(const QString& id) {
  for (QLabel* cover : findChildren<QLabel*>("download_cover")) {
    if (cover->property("cover_id").toString() != id) continue;
    const QString key = cover->parentWidget()->property("download_key").toString();
    if (const DownloadTracker::Entry* entry = tracker_->Find(key)) {
      cover->setPixmap(EntryCover(*tracker_, artwork_, *entry, devicePixelRatioF()));
    }
  }
}

QWidget* DownloadList::BuildRow(const DownloadTracker::Entry& entry, bool first) {
  const theme::Tokens& tokens = theme::Current();
  const QString name = tracker_->NameFor(entry);

  auto* row = new QFrame();
  row->setObjectName("download_row");
  row->setProperty("download_key", entry.key);
  row->setProperty("download_running", entry.state == State::Running);
  row->setProperty("link_state", entry.link_state);
  row->setProperty("first", first);
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
      if (entry.source == "saves") glyph = icons::Glyph::Layers;  // as the game card's saves line
      if (entry.source == "delete" || entry.source == "remove_source") glyph = icons::Glyph::Trash;
    }
    icons::Follow(cover, glyph, 22);
  }
  layout->addWidget(cover);

  auto* text = new QVBoxLayout();
  text->setSpacing(3);
  text->addStretch(1);
  // Elided, and the status below free to shrink: the panel is a fixed width, and a long name or
  // an unbreakable path would otherwise push the buttons past its right edge.
  auto* title = new ElidedLabel(name, row);
  title->setObjectName("download_title");
  title->setStyleSheet("font-weight: 600;");
  title->setToolTip(name);
  text->addWidget(title);

  QString state;
  const char* role = "muted";
  if (entry.state == State::Running) {
    state = RunningText(entry);
  } else if (entry.state == State::Paused && !entry.message.isEmpty()) {
    state = entry.message;
  } else if (entry.state == State::Paused) {
    state = entry.progress >= 0 ? QString("Paused at %1%").arg(qRound(entry.progress * 100))
                                : QString("Paused");
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
  status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  text->addWidget(status);

  // Waiting with nothing on disk yet: no rail, as a busy one would look like it's downloading.
  const bool idle =
      entry.progress < 0 && (entry.link_state == "queued" || entry.link_state == "retrying");
  if ((entry.state == State::Running || entry.state == State::Paused) && !idle) {
    // Busy unless the source reports how far along it is.
    auto* rail = new ProgressRail(row);
    rail->SetProgress(entry.progress);
    text->addWidget(rail);
  }
  text->addStretch(1);
  layout->addLayout(text, /*stretch=*/1);

  // Every row, whatever its state: what it did, or is doing. A link's goes to the daemon's log, so
  // its row keeps to its progress and one button.
  if (entry.kind != Kind::Link) {
    auto* log = new QPushButton("Log", row);
    log->setObjectName("text_button");
    log->setToolTip("Open its live log in a window");
    connect(log, &QPushButton::clicked, this,
            [this, channel = DownloadTracker::LogChannelFor(entry), name] {
              emit LogRequested(channel, name);
            });
    layout->addWidget(log, 0, Qt::AlignVCenter);
  }

  // Side by side on the page; stacked in the narrow popover, Pause over Cancel at the same width.
  QBoxLayout* actions =
      style_ == Style::Full ? static_cast<QBoxLayout*>(new QHBoxLayout()) : new QVBoxLayout();
  actions->setSpacing(style_ == Style::Full ? 8 : 6);
  if (style_ == Style::Compact) actions->addStretch(1);
  const QString game_id = DownloadTracker::GameIdFor(entry);
  const bool tracked = tracker_->game_name && !tracker_->game_name(game_id.toStdString()).isEmpty();
  if (entry.kind == Kind::Link) {
    AddLinkActions(entry, row, actions, text);
  } else if (const QString job = tracker_->JobFor(entry); !job.isEmpty()) {
    if (DownloadTracker::CanPause(entry)) {
      auto* pause = new QPushButton("Pause", row);
      pause->setObjectName("download_pause");
      connect(pause, &QPushButton::clicked, this,
              [this, pause, source = entry.source.toStdString(), ref = entry.ref.toStdString()] {
                pause->setEnabled(false);
                api::PauseStoreInstallAsync(
                    this, source, ref, [this, pause = QPointer(pause)](StoreActionResult result) {
                      if (result.ok) return;
                      if (pause) pause->setEnabled(true);
                      notify::FailedRequest(this, "Could not pause it", result.error);
                    });
              });
      actions->addWidget(pause);
    }
    auto* cancel = new QPushButton("Cancel", row);
    cancel->setObjectName("download_cancel");
    connect(cancel, &QPushButton::clicked, this, [this, cancel, job] {
      cancel->setEnabled(false);  // the row goes once mirad says it's cancelled
      jobs::Cancel(this, job.toStdString(), [this, cancel = QPointer(cancel)](ApiError error) {
        if (error.message.empty()) return;
        if (cancel) cancel->setEnabled(true);
        notify::FailedRequest(this, "Could not cancel it", error);
      });
    });
    actions->addWidget(cancel);
  } else if (entry.state == State::Paused) {
    const std::string source = entry.source.toStdString();
    const std::string ref = entry.ref.toStdString();
    auto* resume = new QPushButton("Resume", row);
    resume->setObjectName("download_resume");
    connect(resume, &QPushButton::clicked, this,
            [this, resume, source, ref, update = entry.update] {
              resume->setEnabled(false);
              api::InstallStoreTitleAsync(
                  this, source, ref, update,
                  [this, resume = QPointer(resume)](StoreActionResult result) {
                    if (result.ok) return;
                    if (resume) resume->setEnabled(true);
                    notify::FailedRequest(this, "Could not resume it", result.error);
                  });
            });
    actions->addWidget(resume);
    auto* discard = new QPushButton("Cancel", row);
    discard->setObjectName("download_cancel");
    connect(discard, &QPushButton::clicked, this, [this, discard, source, ref] {
      discard->setEnabled(false);
      api::DiscardPausedInstallAsync(
          this, source, ref, [this, discard = QPointer(discard)](StoreActionResult result) {
            if (result.ok) return;
            if (discard) discard->setEnabled(true);
            notify::FailedRequest(this, "Could not cancel it", result.error);
          });
    });
    actions->addWidget(discard);
  } else if (entry.state == State::Finished && !game_id.isEmpty() && tracked) {
    auto* show = new QPushButton("Show", row);
    connect(show, &QPushButton::clicked, this, [this, game_id] {
      emit Leaving();
      emit ShowGameRequested(game_id);
    });
    actions->addWidget(show);
  } else if (entry.state == State::Failed) {
    if (const auto action = error_help::ActionFor(entry.error)) {
      auto* fix = new QPushButton(action->label, row);
      connect(fix, &QPushButton::clicked, this, [this, run = action->run] {
        emit Leaving();
        run();
      });
      actions->addWidget(fix);
    }
  }
  if (style_ == Style::Compact) actions->addStretch(1);
  layout->addLayout(actions);
  layout->setAlignment(actions, Qt::AlignVCenter);
  return row;
}

void DownloadList::AddLinkActions(const DownloadTracker::Entry& entry, QWidget* row,
                                  QBoxLayout* actions, QVBoxLayout* text) {
  const QString id = entry.ref;
  const QString& state = entry.link_state;
  const bool full = style_ == Style::Full;
  const auto action = [this, row, actions, id](const QString& label, const QString& name) {
    auto* button = new QPushButton(label, row);
    connect(button, &QPushButton::clicked, this, [this, id, name] { emit LinkAction(id, name); });
    actions->addWidget(button);
  };
  // The way forward first, Pause after it, Cancel last.
  if (state == "paused") action("Resume", "resume");
  if (state == "failed") action("Retry", "retry");
  if (state == "retrying") action("Try now", "retry");
  if (full && state == "queued") action("Move ahead", "front");
  if (state == "downloading" || state == "queued" || state == "service" || state == "retrying")
    action("Pause", "pause");
  if (state == "password" && !full) {
    auto* enter = new QPushButton("Enter password", row);
    connect(enter, &QPushButton::clicked, this, [this, id] {
      emit Leaving();
      emit LinkPasswordRequested(id);
    });
    actions->addWidget(enter);
  }
  if (state == "password" && full) {
    auto* line = new QHBoxLayout();
    line->setSpacing(8);
    auto* password = new QLineEdit(typed_[id], row);
    password->setPlaceholderText("Archive password");
    connect(password, &QLineEdit::textChanged, this,
            [this, id](const QString& typed) { typed_[id] = typed; });
    line->addWidget(password, 1);
    auto* extract = new QPushButton("Extract", row);
    const auto submit = [this, id, password] {
      if (password->text().isEmpty()) return;
      const QString typed = password->text();
      typed_.erase(id);
      emit LinkPasswordEntered(id, typed);
    };
    connect(extract, &QPushButton::clicked, this, submit);
    connect(password, &QLineEdit::returnPressed, this, submit);
    line->addWidget(extract);
    text->insertLayout(text->count() - 1, line);
  }
  if (full && state != "extracting") {
    auto* remove = new QPushButton(state == "done" ? "Clear" : "Cancel", row);
    connect(remove, &QPushButton::clicked, this, [this, id] {
      typed_.erase(id);  // a password typed for it isn't kept
      emit LinkRemoveRequested(id);
    });
    actions->addWidget(remove);
  }
}

}  // namespace mira_gui
