#include "DownloadsPage.h"

#include <QPainter>

#include <algorithm>

#include "../library/ArtworkStore.h"
#include "../library/OwnedTitles.h"
#include "../theme/Theme.h"
#include "BigScreenWindow.h"

namespace mira_gui::bigscreen {
namespace {

constexpr double kMargin = 2.6, kTop = 6.4, kListW = 52, kRowH = 6.6, kHeadH = 3.0;

}  // namespace

DownloadsPage::DownloadsPage(BigScreenWindow* window) : Page(window) {
  connect(window->services().downloads, &DownloadTracker::Changed, this, &DownloadsPage::Refresh);
  connect(window->services().owned_titles, &OwnedTitles::Changed, this, &DownloadsPage::Refresh);
  connect(window->services().artwork, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
}

void DownloadsPage::Shown() { Refresh(); }

void DownloadsPage::Refresh() {
  items_.clear();
  for (const DownloadTracker::Entry& entry : window_->services().downloads->Entries()) {
    const bool live = entry.state == DownloadTracker::State::Running || entry.state == DownloadTracker::State::Paused;
    if (!live || (entry.kind != DownloadTracker::Kind::Title && entry.kind != DownloadTracker::Kind::Game)) continue;
    const QString key = entry.kind == DownloadTracker::Kind::Game ? entry.ref : entry.source + "-" + entry.ref;
    Item item = window_->Find(key);
    if (item.key.isEmpty()) {
      item = {key, window_->services().downloads->NameFor(entry), entry.source, entry.ref, std::nullopt};
    }
    // A title's install is tracked by source and ref even once the game exists.
    if (entry.kind == DownloadTracker::Kind::Title) item.ref = entry.ref;
    items_.push_back(std::move(item));
  }
  installs_ = int(items_.size());
  std::vector<Item> ready;
  for (const StoreTitle& title : window_->services().owned_titles->Titles()) {
    if (!title.owned || title.installed || !window_->Browsable(title)) continue;
    Item item = window_->ItemFor(title);
    if (window_->Download(item) == nullptr) ready.push_back(std::move(item));
  }
  std::ranges::sort(ready, [](const Item& a, const Item& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
  std::ranges::move(ready, std::back_inserter(items_));
  focus_ = std::clamp(focus_, 0, std::max(0, int(items_.size()) - 1));
  update();
  emit HintsChanged();
}

double DownloadsPage::RowTop(int i) const {
  // A heading above each section.
  double top = kTop + 3.2 + double(i) * (kRowH + 0.5);
  if (installs_ > 0) top += kHeadH;
  if (i >= installs_ && installs_ < int(items_.size())) top += kHeadH;
  return top;
}

bool DownloadsPage::Navigate(Nav nav) {
  if (items_.empty()) return false;
  const Item& item = items_[size_t(focus_)];
  const DownloadTracker::Entry* entry = window_->Download(item);
  const bool ready = focus_ >= installs_;
  switch (nav) {
    case Nav::Up: focus_ = std::max(0, focus_ - 1); break;
    case Nav::Down: focus_ = std::min(int(items_.size()) - 1, focus_ + 1); break;
    case Nav::Accept:
      if (ready) {
        window_->Install(item);
        return true;
      }
      if (entry == nullptr) return true;
      if (entry->state == DownloadTracker::State::Paused) {
        window_->Resume(item);
      } else if (DownloadTracker::CanPause(*entry)) {
        window_->Pause(item);
      }
      return true;
    case Nav::Action:
      if (ready) window_->OpenGame(item);
      else window_->CancelInstall(item);
      return true;
    default: return false;
  }
  update();
  emit HintsChanged();
  return true;
}

QList<Hint> DownloadsPage::Hints() const {
  if (items_.empty()) return {{Nav::Back, "Back"}};
  QList<Hint> hints;
  if (focus_ >= installs_) return {{Nav::Accept, "Install"}, {Nav::Action, "Details"}, {Nav::Back, "Back"}};
  const DownloadTracker::Entry* entry = window_->Download(items_[size_t(focus_)]);
  if (entry && entry->state == DownloadTracker::State::Paused) hints.append({Nav::Accept, "Resume"});
  else if (entry && DownloadTracker::CanPause(*entry)) hints.append({Nav::Accept, "Pause"});
  hints.append({Nav::Action, "Cancel"});
  hints.append({Nav::Back, "Back"});
  return hints;
}

void DownloadsPage::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  painter.fillRect(rect(), QColor(tokens.window.red(), tokens.window.green(), tokens.window.blue(), 170));
  painter.setFont(Font(u, 2.0, QFont::ExtraBold));
  painter.setPen(tokens.text);
  painter.drawText(QPointF(kMargin * u, (kTop + 1.6) * u), "Downloads");
  const double list_w = std::min(kListW * u, width() - kMargin * u * 2);
  if (items_.empty()) {
    painter.setFont(Font(u, 1.05));
    painter.setPen(tokens.text_muted);
    painter.drawText(QRectF(kMargin * u, (kTop + 3.2) * u, list_w, 6 * u), Qt::TextWordWrap,
                     "Nothing is downloading, and every game you own is installed.");
    return;
  }
  // Scrolled so the focused row stays on screen.
  const double bottom = height() / u - 4.0;
  const double scroll = std::max(0.0, RowTop(focus_) + kRowH - bottom);
  const auto heading = [&](double top, const QString& text) {
    painter.setFont(Font(u, 1.2, QFont::Bold));
    painter.setPen(tokens.text_muted);
    painter.drawText(QPointF(kMargin * u, (top - scroll - 0.9) * u), text);
  };
  painter.save();
  painter.setClipRect(QRectF(0, (kTop + 2.4) * u, width(), height()));
  if (installs_ > 0) heading(RowTop(0), "Installing");
  if (installs_ < int(items_.size())) heading(RowTop(installs_), "Ready to install");
  const QSize thumb(qRound(3.4 * u), qRound(5.1 * u));
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item& item = items_[i];
    const DownloadTracker::Entry* entry = window_->Download(item);
    const QRectF row(kMargin * u, (RowTop(int(i)) - scroll) * u, list_w, kRowH * u);
    if (row.bottom() < 0) continue;
    if (row.top() > height()) break;
    const bool focused = int(i) == focus_;
    painter.setPen(focused ? QPen(Accent(), u * 0.12) : Qt::NoPen);
    painter.setBrush(focused ? tokens.surface_alt : tokens.surface);
    painter.drawRoundedRect(row, u * 0.5, u * 0.5);
    DrawCover(painter, QRectF(row.left() + u * 0.75, row.top() + u * 0.75, thumb.width(), thumb.height()),
              window_->Cover(item, thumb), u * 0.5, false, false, -1);
    const double text_x = row.left() + u * 5.2;
    painter.setFont(Font(u, 1.1, QFont::Bold));
    painter.setPen(tokens.text);
    painter.drawText(QPointF(text_x, row.top() + u * 2.0), item.name);
    if (entry == nullptr) {
      painter.setFont(Font(u, 0.88));
      painter.setPen(tokens.text_muted);
      painter.drawText(QPointF(text_x, row.top() + u * 3.4), SourceName(item.source));
      continue;
    }
    const bool paused = entry->state == DownloadTracker::State::Paused;
    painter.setFont(Font(u, 0.88));
    painter.setPen(tokens.text_muted);
    painter.drawText(QPointF(text_x, row.top() + u * 3.4),
                     paused ? StatusText(item, std::max(0.0, entry->progress), true) : DownloadTracker::ProgressText(*entry));
    const QRectF bar(text_x, row.top() + u * 4.4, row.right() - text_x - u * 1.2, u * 0.4);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 28));
    painter.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
    painter.setBrush(paused ? tokens.status_missing : Accent());
    painter.drawRoundedRect(QRectF(bar.topLeft(), QSizeF(bar.width() * std::max(0.0, entry->progress), bar.height())),
                            bar.height() / 2, bar.height() / 2);
  }
  painter.restore();
}

}  // namespace mira_gui::bigscreen
