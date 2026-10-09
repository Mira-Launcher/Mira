#include "HomePage.h"

#include <QPainter>

#include <algorithm>
#include <map>
#include <ranges>

#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"
#include "../library/GamePresentation.h"
#include "../theme/Theme.h"
#include "BigScreenWindow.h"

namespace mira_gui::bigscreen {
namespace {

// In units (see Unit).
constexpr double kMargin = 2.6;
constexpr double kInfoTop = 6.4;
constexpr double kRowsTop = 23.2;
constexpr double kRowHeight = 18.6;
constexpr double kTileW = 10, kTileH = 15, kTileGap = 1.1;

bool Browsable(const GameSummary& game) { return !IsHidden(game) && !IsApp(game); }

bool ByName(const Item& a, const Item& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; }

}  // namespace

HomePage::HomePage(BigScreenWindow* window) : Page(window) {
  animation_.setInterval(16);
  connect(&animation_, &QTimer::timeout, this, &HomePage::Animate);
  // Bursts of library events become one rebuild.
  rebuild_.setSingleShot(true);
  rebuild_.setInterval(50);
  connect(&rebuild_, &QTimer::timeout, this, &HomePage::Rebuild);
  const LibraryServices& services = window->services();
  connect(services.library, &GameLibraryModel::Changed, &rebuild_, qOverload<>(&QTimer::start));
  connect(services.downloads, &DownloadTracker::Changed, this, [this] {
    update();
    emit HintsChanged();
  });
  connect(services.artwork, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
  Rebuild();
}

void HomePage::Shown() {
  Rebuild();
  FocusChanged();
}

void HomePage::Rebuild() {
  // Keep each row's focused game where it was.
  std::map<QString, QString> focused;
  for (const Row& row : rows_) {
    if (!row.items.empty()) focused[row.label] = row.items[size_t(row.focus)].key;
  }
  const QString focused_row = rows_.empty() ? QString() : rows_[size_t(row_)].label;

  const LibraryServices& services = window_->services();
  std::vector<Row> rows;
  Row recent{"Continue playing"};
  for (const GameSummary* game : services.library->RecentlyPlayed(12)) {
    if (Browsable(*game) && (game->last_played_at || game->running)) recent.items.push_back(window_->ItemFor(*game));
  }
  Row installed{"Installed"};
  Row pinned{"Pinned"};
  // A row per source, then per tag, sorted by name.
  std::map<QString, Row> by_source, by_tag;
  for (const GameSummary& game : services.library->Games()) {
    if (!Browsable(game)) continue;
    const Item item = window_->ItemFor(game);
    installed.items.push_back(item);
    if (IsPinned(game)) pinned.items.push_back(item);
    const QString source = SourceName(QString::fromStdString(game.source));
    by_source.try_emplace(source, Row{source}).first->second.items.push_back(item);
    for (const std::string& tag : game.tags) {
      if (IsMeaningTag(tag)) continue;
      const QString name = QString::fromStdString(tag);
      by_tag.try_emplace(name, Row{name}).first->second.items.push_back(item);
    }
  }
  // Owned games that aren't installed are on the Downloads tab, not here.
  std::vector<Row*> order{&recent, &pinned};
  for (auto& [name, row] : by_tag) order.push_back(&row);
  // One source would only repeat Installed.
  if (by_source.size() > 1) {
    for (auto& [name, row] : by_source) order.push_back(&row);
  }
  order.push_back(&installed);
  for (Row* row : order) SortRow(*row);
  for (Row* row : order) {
    if (row->items.empty()) continue;
    if (const auto found = focused.find(row->label); found != focused.end()) {
      const auto at = std::ranges::find(row->items, found->second, &Item::key);
      if (at != row->items.end()) row->focus = int(at - row->items.begin());
    }
    for (const Row& old : rows_) {
      if (old.label == row->label) row->scroll = old.scroll;
    }
    rows.push_back(std::move(*row));
  }
  rows_ = std::move(rows);
  row_ = 0;
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].label == focused_row) row_ = int(i);
  }
  FocusChanged();
}

HomePage::Order HomePage::OrderOf(const Row& row) const {
  // Continue playing is most recent first unless changed; the rest go by name.
  return orders_.value(row.label, row.label == "Continue playing" ? Order::Recent : Order::Name);
}

void HomePage::SortRow(Row& row) const {
  const Order order = OrderOf(row);
  const auto played = [](const Item& item) { return item.game ? item.game->play_seconds : 0; };
  const auto last = [](const Item& item) { return item.game ? item.game->last_played_at.value_or(0) : 0; };
  switch (order) {
    case Order::Name: std::ranges::stable_sort(row.items, ByName); break;
    case Order::Recent: std::ranges::stable_sort(row.items, std::greater{}, last); break;
    case Order::MostPlayed: std::ranges::stable_sort(row.items, std::greater{}, played); break;
    default: break;
  }
}

const Item* HomePage::Focused() const {
  if (rows_.empty()) return nullptr;
  const Row& row = rows_[size_t(row_)];
  return &row.items[size_t(row.focus)];
}

void HomePage::FocusChanged() {
  if (const Item* item = Focused()) window_->ShowHero(*item);
  animation_.start();
  update();
  emit HintsChanged();
}

bool HomePage::Navigate(Nav nav) {
  if (rows_.empty()) return false;
  Row& row = rows_[size_t(row_)];
  switch (nav) {
    case Nav::Left:
    case Nav::Right: {
      const int next = std::clamp(row.focus + (nav == Nav::Right ? 1 : -1), 0, int(row.items.size()) - 1);
      if (next == row.focus) {
        window_->Bump();
        return true;
      }
      row.focus = next;
      break;
    }
    case Nav::Up:
    case Nav::Down: {
      const int next = std::clamp(row_ + (nav == Nav::Down ? 1 : -1), 0, int(rows_.size()) - 1);
      if (next == row_) {
        window_->Bump();
        return true;
      }
      row_ = next;
      break;
    }
    case Nav::Accept:
      // Mid-download there's nothing to play, so A opens the game instead.
      if (window_->QuickActionLabel(*Focused()).isEmpty()) window_->OpenGame(*Focused());
      else window_->QuickAction(*Focused());
      return true;
    case Nav::Action: window_->OpenGame(*Focused()); return true;
    case Nav::Sort: {
      const QString key = row.items[size_t(row.focus)].key;
      orders_[row.label] = Order((int(OrderOf(row)) + 1) % int(Order::kCount));
      SortRow(row);
      // The same game stays focused, wherever it moved to.
      const auto at = std::ranges::find(row.items, key, &Item::key);
      row.focus = at == row.items.end() ? 0 : int(at - row.items.begin());
      break;
    }
    default: return false;
  }
  FocusChanged();
  return true;
}

QList<Hint> HomePage::Hints() const {
  QList<Hint> hints;
  if (const Item* item = Focused()) {
    const QString quick = window_->QuickActionLabel(*item);
    hints.append({Nav::Accept, quick.isEmpty() ? "Details" : quick});
    hints.append({Nav::Action, "Details"});
    static const char* const kOrderNames[] = {"A–Z", "Recent", "Most played"};
    const Order next = Order((int(OrderOf(rows_[size_t(row_)])) + 1) % int(Order::kCount));
    hints.append({Nav::Sort, QString("Sort: ") + kOrderNames[int(next)]});
  }
  hints.append({Nav::Search, "Search"});
  return hints;
}

double HomePage::TargetScroll(const Row& row) const {
  const double u = window_->unit();
  const double step = (kTileW + kTileGap) * u;
  const double content = double(row.items.size()) * step - kTileGap * u + kMargin * u * 2;
  const double max = std::max(0.0, content - width());
  // One tile stays visible to the left of the focused one.
  return std::clamp((row.focus - 1) * step, 0.0, max);
}

void HomePage::Animate() {
  bool moving = false;
  const auto ease = [&moving](double& value, double target, double snap) {
    if (std::abs(target - value) < snap) {
      value = target;
    } else {
      value += (target - value) * 0.25;
      moving = true;
    }
  };
  ease(row_scroll_, row_, 0.002);
  for (Row& row : rows_) ease(row.scroll, TargetScroll(row), 0.5);
  update();
  if (!moving) animation_.stop();
}

void HomePage::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  const Item* focused = Focused();
  if (focused == nullptr) {
    painter.setPen(tokens.text_muted);
    painter.setFont(Font(u, 1.3));
    painter.drawText(rect(), Qt::AlignCenter, "No games yet. Add some from the desktop window.");
    return;
  }

  // The focused game: name, status, how much it was played.
  const double info_w = std::min(44 * u, width() - kMargin * u * 2);
  painter.setFont(Font(u, 3.1, QFont::ExtraBold));
  painter.setPen(tokens.text);
  const QRectF title_box(kMargin * u, kInfoTop * u, info_w, 7.2 * u);
  const QRectF title_used = painter.boundingRect(title_box, Qt::TextWordWrap, focused->name);
  painter.drawText(title_box, Qt::TextWordWrap, focused->name);
  const double progress = window_->Progress(*focused);
  const DownloadTracker::Entry* download = window_->Download(*focused);
  const bool paused = download && download->state == DownloadTracker::State::Paused;
  QPointF at(kMargin * u, std::min(title_used.bottom(), title_box.bottom()) + u * 0.9);
  at.rx() += DrawPill(painter, at, u, StatusText(*focused, progress, paused), StatusColor(*focused, progress)) + u * 0.5;
  at.rx() += DrawPill(painter, at, u, SourceName(focused->source)) + u * 0.9;
  painter.setFont(Font(u, 0.95));
  painter.setPen(tokens.text_muted);
  QStringList meta;
  if (focused->game) {
    if (focused->game->play_seconds > 0) meta << FormatPlaytime(focused->game->play_seconds);
    meta << FormatPlayedAgo(focused->game->last_played_at);
  }
  painter.drawText(QPointF(at.x(), at.y() + u * 1.05), meta.join("   ·   "));

  // The rows, the focused one at kRowsTop.
  painter.setClipRect(QRectF(0, (kRowsTop - 1.5) * u, width(), height()));
  const QSize tile(qRound(kTileW * u), qRound(kTileH * u));
  for (size_t r = 0; r < rows_.size(); ++r) {
    const Row& row = rows_[r];
    const double offset = double(r) - row_scroll_;
    const double top = (kRowsTop + offset * kRowHeight) * u;
    if (top > height() || offset < -1) continue;
    const bool active = int(r) == row_;
    painter.setOpacity(offset < 0 ? std::max(0.0, 1 + offset * 2) : active ? 1.0 : 0.55);
    painter.setFont(Font(u, 1.15, QFont::Bold));
    painter.setPen(tokens.text);
    painter.drawText(QPointF(kMargin * u, top + u * 1.0), row.label);
    const double label_w = painter.fontMetrics().horizontalAdvance(row.label);
    painter.setFont(Font(u, 0.9));
    painter.setPen(tokens.text_muted);
    painter.drawText(QPointF(kMargin * u + label_w + u * 0.6, top + u * 1.0), QString::number(row.items.size()));
    const double tiles_top = top + u * 2.3;
    for (size_t i = 0; i < row.items.size(); ++i) {
      const double x = kMargin * u + double(i) * (kTileW + kTileGap) * u - row.scroll;
      if (x > width() || x + tile.width() < 0) continue;
      const Item& item = row.items[i];
      const bool focus = active && int(i) == row.focus;
      QRectF box(x, tiles_top, tile.width(), tile.height());
      if (focus) box = QRectF(box.center() - QPointF(box.width(), box.height()) * 0.54, box.size() * 1.08);
      DrawCover(painter, box, window_->Cover(item, tile), u, focus, !item.installed(), window_->Progress(item));
    }
  }
}

}  // namespace mira_gui::bigscreen
