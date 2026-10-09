#include "DownloadsPage.h"

#include <QPainter>

#include <algorithm>

#include "../library/ArtworkStore.h"
#include "../library/OwnedTitles.h"
#include "../theme/Theme.h"
#include "BigScreenWindow.h"

namespace mira_gui::bigscreen {
namespace {

constexpr double kMargin = 2.6, kTop = 6.4, kHeroH = 15, kRowH = 5.4, kHeadH = 3.0, kGap = 1.2;
constexpr int kColumns = 6;

// Moves `index` one step through a grid `columns` wide; false at an edge.
bool MoveInGrid(int& index, Nav nav, int columns, int count) {
  int next = index;
  switch (nav) {
    case Nav::Left: next = index % columns > 0 ? index - 1 : -1; break;
    case Nav::Right: next = index % columns < columns - 1 ? index + 1 : count; break;
    case Nav::Up: next = index - columns; break;
    case Nav::Down: next = index + columns; break;
    default: return false;
  }
  if (next < 0 || next >= count) return false;
  index = next;
  return true;
}

}  // namespace

DownloadsPage::DownloadsPage(BigScreenWindow* window) : Page(window) {
  connect(window->services().downloads, &DownloadTracker::Changed, this, &DownloadsPage::Refresh);
  connect(window->services().owned_titles, &OwnedTitles::Changed, this, &DownloadsPage::Refresh);
  connect(window->services().artwork, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
  connect(window->services().artwork, &ArtworkStore::RequestsDropped, this, qOverload<>(&QWidget::update));
}

void DownloadsPage::Shown() {
  Refresh();
  FocusChanged();
}

void DownloadsPage::Refresh() {
  const QString focused = items_.empty() ? QString() : items_[size_t(focus_)].key;
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
  // The running one leads, over the paused.
  std::ranges::stable_sort(items_, [this](const Item& a, const Item& b) {
    const auto running = [this](const Item& item) {
      const DownloadTracker::Entry* entry = window_->Download(item);
      return entry != nullptr && entry->state == DownloadTracker::State::Running;
    };
    return running(a) && !running(b);
  });
  installs_ = int(items_.size());
  std::vector<Item> ready;
  for (const StoreTitle& title : window_->services().owned_titles->Titles()) {
    if (!title.owned || title.installed || !window_->Browsable(title)) continue;
    Item item = window_->ItemFor(title);
    if (window_->Download(item) == nullptr) ready.push_back(std::move(item));
  }
  std::ranges::sort(ready, [](const Item& a, const Item& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
  std::ranges::move(ready, std::back_inserter(items_));
  // Keep the focus on the same game when the list moves under it.
  const auto at = std::ranges::find(items_, focused, &Item::key);
  focus_ = at != items_.end() ? int(at - items_.begin()) : std::clamp(focus_, 0, std::max(0, int(items_.size()) - 1));
  // The focused install finished: the hero follows the game that took its place.
  if (!items_.empty() && items_[size_t(focus_)].key != focused) window_->ShowHero(items_[size_t(focus_)], this);
  update();
  emit HintsChanged();
}

void DownloadsPage::FocusChanged() {
  if (!items_.empty()) window_->ShowHero(items_[size_t(focus_)], this);
  update();
  emit HintsChanged();
}

bool DownloadsPage::Navigate(Nav nav) {
  if (items_.empty()) return false;
  const Item& item = items_[size_t(focus_)];
  const DownloadTracker::Entry* entry = window_->Download(item);
  const bool ready = focus_ >= installs_;
  const int count = int(items_.size()) - installs_;
  switch (nav) {
    case Nav::Up:
    case Nav::Down:
    case Nav::Left:
    case Nav::Right:
      if (ready) {
        int index = focus_ - installs_;
        if (MoveInGrid(index, nav, kColumns, count)) {
          focus_ = installs_ + index;
        } else if (nav == Nav::Up && installs_ > 0) {
          focus_ = installs_ - 1;
        } else {
          window_->Bump();
          return true;
        }
      } else if (nav == Nav::Up && focus_ > 0) {
        --focus_;
      } else if (nav == Nav::Down && focus_ + 1 < int(items_.size())) {
        ++focus_;
      } else {
        window_->Bump();
        return true;
      }
      break;
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
  FocusChanged();
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

DownloadsPage::Layout DownloadsPage::Arrange() const {
  const double u = window_->unit();
  const double left = kMargin * u, full_w = width() - kMargin * u * 2;
  Layout layout;
  double y = (kTop + 3.2) * u;
  for (int i = 0; i < installs_; ++i) {
    if (i == 1) {
      layout.headings.emplace_back(y + kHeadH * u * 0.6, "Up next");
      y += kHeadH * u;
    }
    const double h = (i == 0 ? kHeroH : kRowH) * u;
    layout.boxes.emplace_back(left, y, full_w, h);
    y += h + kGap * u;
  }
  if (installs_ < int(items_.size())) {
    if (installs_ > 0) y += u;
    layout.headings.emplace_back(y + kHeadH * u * 0.6, QString("Ready to install · %1").arg(int(items_.size()) - installs_));
    y += kHeadH * u;
    const double gap = kGap * u;
    // Small enough that a row and a half shows below the heading.
    const double tile_h = std::min((full_w - gap * (kColumns - 1)) / kColumns * 1.5, height() * 0.42);
    const double tile_w = tile_h / 1.5;
    const double grid_left = left + (full_w - (tile_w * kColumns + gap * (kColumns - 1))) / 2;
    for (int i = 0; i < int(items_.size()) - installs_; ++i) {
      layout.boxes.emplace_back(grid_left + (i % kColumns) * (tile_w + gap), y + (i / kColumns) * (tile_h + gap), tile_w, tile_h);
    }
  }
  return layout;
}

void DownloadsPage::PaintInstall(QPainter& painter, const QRectF& box, const Item& item, bool hero, bool focused) const {
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  const DownloadTracker::Entry* entry = window_->Download(item);
  const bool paused = entry != nullptr && entry->state == DownloadTracker::State::Paused;
  const double progress = entry != nullptr ? std::max(0.0, entry->progress) : 0.0;
  painter.setPen(focused ? QPen(Accent(), u * 0.15) : Qt::NoPen);
  painter.setBrush(QColor(tokens.surface.red(), tokens.surface.green(), tokens.surface.blue(), hero ? 215 : 235));
  painter.drawRoundedRect(box, u * 0.6, u * 0.6);
  const double pad = (hero ? 1.4 : 0.7) * u;
  const QRectF cover(box.left() + pad, box.top() + pad, (box.height() - pad * 2) / 1.5, box.height() - pad * 2);
  DrawCover(painter, cover, window_->Cover(item, cover.size().toSize()), u * (hero ? 1.0 : 0.5), false, false, -1);
  const double text_x = cover.right() + pad * (hero ? 1.4 : 1.6);
  const double text_w = box.right() - text_x - pad * 1.4;

  // Percent on the right; the name and the rest take what's left.
  const QString percent = entry != nullptr && entry->progress >= 0 ? QString("%1%").arg(qRound(progress * 100)) : QString();
  painter.setFont(Font(u, hero ? 2.6 : 1.2, QFont::ExtraBold));
  const double percent_w = percent.isEmpty() ? 0 : painter.fontMetrics().horizontalAdvance(percent) + u;
  painter.setPen(paused ? tokens.text_muted : tokens.text);
  const double name_base = box.top() + (hero ? 4.2 : 2.2) * u;
  if (!percent.isEmpty()) painter.drawText(QRectF(text_x, box.top(), text_w, (hero ? 5.6 : 3.0) * u), Qt::AlignRight | Qt::AlignVCenter, percent);

  painter.setFont(Font(u, hero ? 2.0 : 1.1, QFont::Bold));
  painter.setPen(tokens.text);
  painter.drawText(QPointF(text_x, name_base), painter.fontMetrics().elidedText(item.name, Qt::ElideRight, int(text_w - percent_w)));
  painter.setFont(Font(u, hero ? 1.05 : 0.88));
  painter.setPen(tokens.text_muted);
  const QString status = paused ? "Paused" : entry != nullptr ? DownloadTracker::ProgressText(*entry) : QString();
  const QString line = SourceName(item.source) + (status.isEmpty() ? QString() : "  ·  " + status);
  painter.drawText(QPointF(text_x, name_base + (hero ? 2.2 : 1.5) * u),
                   painter.fontMetrics().elidedText(line, Qt::ElideRight, int(text_w)));

  const double bar_h = (hero ? 0.75 : 0.4) * u;
  const QRectF bar(text_x, box.bottom() - pad - bar_h - (hero ? 1.2 : 0.2) * u, text_w, bar_h);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(255, 255, 255, 28));
  painter.drawRoundedRect(bar, bar_h / 2, bar_h / 2);
  painter.setBrush(paused ? tokens.status_missing : Accent());
  painter.drawRoundedRect(QRectF(bar.topLeft(), QSizeF(bar.width() * progress, bar_h)), bar_h / 2, bar_h / 2);
}

void DownloadsPage::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  // Lighter than other pages, so the download's art shows through.
  painter.fillRect(rect(), QColor(tokens.window.red(), tokens.window.green(), tokens.window.blue(), installs_ > 0 ? 120 : 170));
  painter.setFont(Font(u, 2.0, QFont::ExtraBold));
  painter.setPen(tokens.text);
  painter.drawText(QPointF(kMargin * u, (kTop + 1.6) * u), "Downloads");
  if (items_.empty()) {
    painter.setFont(Font(u, 1.05));
    painter.setPen(tokens.text_muted);
    painter.drawText(QRectF(kMargin * u, (kTop + 3.2) * u, width() - kMargin * u * 2, 6 * u), Qt::TextWordWrap,
                     "Nothing is downloading, and every game you own is installed.");
    return;
  }
  const Layout layout = Arrange();
  // Scrolled so the focused one sits above the hints.
  const QRectF& focused = layout.boxes[size_t(focus_)];
  const double scroll = std::max(0.0, focused.bottom() - (height() - 4.5 * u));
  painter.save();
  painter.setClipRect(QRectF(0, (kTop + 2.4) * u, width(), height()));
  painter.translate(0, -scroll);
  painter.setFont(Font(u, 1.2, QFont::Bold));
  painter.setPen(tokens.text_muted);
  for (const auto& [y, text] : layout.headings) painter.drawText(QPointF(kMargin * u, y), text);
  for (size_t i = 0; i < items_.size(); ++i) {
    const QRectF& box = layout.boxes[i];
    if (box.bottom() < scroll || box.top() > scroll + height()) continue;
    const bool is_focused = int(i) == focus_;
    if (int(i) < installs_) {
      PaintInstall(painter, box, items_[i], i == 0, is_focused);
    } else {
      DrawCover(painter, box, window_->Cover(items_[i], box.size().toSize()), u, is_focused, false, -1);
    }
  }
  painter.restore();
  // The focused game's name under the grid's heading, since covers alone may not say.
  if (focus_ >= installs_) {
    painter.setFont(Font(u, 1.0, QFont::DemiBold));
    painter.setPen(tokens.text);
    const QString name = items_[size_t(focus_)].name + "  ·  " + SourceName(items_[size_t(focus_)].source);
    painter.drawText(QRectF(kMargin * u, height() - 4.3 * u, width() * 0.45, 2.5 * u), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(name, Qt::ElideRight, int(width() * 0.45)));
  }
}

}  // namespace mira_gui::bigscreen
