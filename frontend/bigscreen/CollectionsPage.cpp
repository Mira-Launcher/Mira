#include "CollectionsPage.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"
#include "../theme/Theme.h"
#include "BigScreenWindow.h"

namespace mira_gui::bigscreen {
namespace {

constexpr double kMargin = 2.6, kTop = 6.4, kCardTop = 9.6, kCardW = 20, kCardGap = 1.2;
constexpr int kCards = 4, kResultColumns = 5;

bool IsDirection(Nav nav) { return nav == Nav::Up || nav == Nav::Down || nav == Nav::Left || nav == Nav::Right; }

// Moves `index` one step through a grid `columns` wide; false at an edge.
bool Move(int& index, Nav nav, int columns, int count) {
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

CollectionsPage::CollectionsPage(BigScreenWindow* window) : Page(window) {
  const LibraryServices& services = window->services();
  connect(services.library, &GameLibraryModel::Changed, this, &CollectionsPage::Rebuild);
  connect(services.artwork, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
}

void CollectionsPage::Shown() { Rebuild(); }

void CollectionsPage::Rebuild() {
  // Keep the focus on the same tag and game across the rebuild.
  const QString tag = collections_.empty() ? QString() : Current().tag;
  const QString key = collections_.empty() ? QString() : Current().items[size_t(game_)].key;

  collections_.clear();
  for (const GameSummary& game : window_->services().library->Games()) {
    if (!window_->Browsable(game)) continue;
    const Item item = window_->ItemFor(game);
    for (const std::string& tag_name : game.tags) {
      if (IsMeaningTag(tag_name)) continue;
      const QString name = QString::fromStdString(tag_name);
      auto found = std::ranges::find_if(collections_, [&](const Collection& c) { return c.tag.compare(name, Qt::CaseInsensitive) == 0; });
      if (found == collections_.end()) found = collections_.insert(collections_.end(), Collection{name, {}});
      if (std::ranges::none_of(found->items, [&](const Item& i) { return i.key == item.key; })) found->items.push_back(item);
    }
  }
  std::ranges::sort(collections_, [](const Collection& a, const Collection& b) { return a.tag.compare(b.tag, Qt::CaseInsensitive) < 0; });
  for (Collection& collection : collections_) {
    std::ranges::sort(collection.items, [](const Item& a, const Item& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
  }

  const auto found = std::ranges::find_if(collections_, [&](const Collection& c) { return c.tag.compare(tag, Qt::CaseInsensitive) == 0; });
  collection_ = found == collections_.end() ? 0 : int(found - collections_.begin());
  if (found == collections_.end()) open_ = false;
  game_ = 0;
  if (!collections_.empty()) {
    const std::vector<Item>& items = Current().items;
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].key == key) game_ = int(i);
    }
  }
  update();
  emit HintsChanged();
}

const CollectionsPage::Collection& CollectionsPage::Current() const { return collections_[size_t(collection_)]; }

int CollectionsPage::Columns() const {
  const double u = window_->unit();
  const int fit = int((width() - 2 * kMargin * u + kCardGap * u) / ((kCardW + kCardGap) * u));
  return std::clamp(fit, 1, kCards);
}

bool CollectionsPage::Navigate(Nav nav) {
  if (collections_.empty()) return false;
  if (IsDirection(nav)) {
    const bool in_tags = !open_;
    const int count = int(in_tags ? collections_.size() : Current().items.size());
    int& index = in_tags ? collection_ : game_;
    if (!Move(index, nav, in_tags ? Columns() : kResultColumns, count)) window_->Bump();
  } else if (nav == Nav::Accept && !open_) {
    open_ = true;
    game_ = 0;
  } else if (nav == Nav::Accept) {
    const Item& item = Current().items[size_t(game_)];
    if (window_->QuickActionLabel(item).isEmpty()) {
      window_->OpenGame(item);
    } else {
      window_->QuickAction(item);
    }
    return true;
  } else if (nav == Nav::Action && open_) {
    window_->OpenGame(Current().items[size_t(game_)]);
    return true;
  } else if (nav == Nav::Back && open_) {
    open_ = false;
  } else {
    return false;
  }
  window_->ShowHero(open_ ? Current().items[size_t(game_)] : Current().items.front());
  update();
  emit HintsChanged();
  return true;
}

QList<Hint> CollectionsPage::Hints() const {
  if (collections_.empty()) return {};
  if (!open_) return {{Nav::Accept, "Open"}};
  const QString label = window_->QuickActionLabel(Current().items[size_t(game_)]);
  return {{Nav::Accept, label.isEmpty() ? QString("Details") : label}, {Nav::Action, "Details"}, {Nav::Back, "Collections"}};
}

void CollectionsPage::DrawCard(QPainter& painter, const QRectF& box, const Collection& collection, bool focused) const {
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  painter.save();
  if (focused) {
    painter.translate(box.center());
    painter.scale(1.04, 1.04);
    painter.translate(-box.center());
  }
  const double radius = u * 0.6;
  QPainterPath card;
  card.addRoundedRect(box, radius, radius);
  painter.setPen(focused ? QPen(Accent(), u * 0.15) : Qt::NoPen);
  painter.setBrush(tokens.surface);
  painter.drawPath(card);
  painter.setClipPath(card);

  // A 2x2 collage of the first covers over the top three quarters.
  const QRectF collage(box.left(), box.top(), box.width(), box.height() * 0.75);
  painter.setPen(Qt::NoPen);
  painter.setBrush(tokens.surface_alt);
  painter.drawRect(collage);
  const double cell_w = collage.width() / 2, cell_h = collage.height() / 2;
  const int shown = std::min(kCards, int(collection.items.size()));
  for (int i = 0; i < shown; ++i) {
    const QRectF cell(collage.left() + (i % 2) * cell_w, collage.top() + (i / 2) * cell_h, cell_w, cell_h);
    const QSize size = cell.size().toSize();
    const QPixmap cover = window_->Cover(collection.items[size_t(i)], size);
    if (cover.isNull()) continue;
    const QPixmap scaled = cover.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QPointF crop((scaled.width() - size.width()) / 2.0, (scaled.height() - size.height()) / 2.0);
    painter.drawPixmap(cell, scaled, QRectF(crop, cell.size()));
  }

  const double text_x = box.left() + u * 0.9;
  const double text_w = box.width() - u * 1.8;
  painter.setPen(tokens.text);
  painter.setFont(Font(u, 1.1, QFont::Bold));
  painter.drawText(QPointF(text_x, collage.bottom() + u * 1.9),
                   painter.fontMetrics().elidedText(collection.tag, Qt::ElideRight, qRound(text_w)));
  const int count = int(collection.items.size());
  painter.setFont(Font(u, 0.9));
  painter.setPen(tokens.text_muted);
  painter.drawText(QPointF(text_x, collage.bottom() + u * 3.2), QString("%1 %2").arg(count).arg(count == 1 ? "game" : "games"));
  painter.restore();
}

void CollectionsPage::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  painter.fillRect(rect(), QColor(tokens.window.red(), tokens.window.green(), tokens.window.blue(), 170));
  painter.setFont(Font(u, 2.0, QFont::ExtraBold));
  painter.setPen(tokens.text);
  if (collections_.empty()) {
    painter.drawText(QPointF(kMargin * u, (kTop + 1.6) * u), "Collections");
    painter.setFont(Font(u, 1.05));
    painter.setPen(tokens.text_muted);
    painter.drawText(QRectF(kMargin * u, (kTop + 3.2) * u, width() - kMargin * u * 2, 6 * u), Qt::AlignCenter | Qt::TextWordWrap,
                     "No collections yet. Tag games in the desktop window and they show up here.");
    return;
  }

  const Collection& current = Current();
  if (!open_) {
    painter.drawText(QPointF(kMargin * u, (kTop + 1.6) * u), "Collections");
    // Cards, scrolled so the focused row stays in view.
    const double card_w = kCardW * u, card_h = card_w * 0.75, gap = kCardGap * u;
    const int columns = Columns();
    const int first_row = std::max(0, collection_ / columns - 1);
    painter.setClipRect(QRectF(0, kCardTop * u - u * 0.5, width(), height()));
    for (size_t i = 0; i < collections_.size(); ++i) {
      const int row = int(i) / columns - first_row;
      if (row < 0) continue;
      const QRectF box(kMargin * u + double(int(i) % columns) * (card_w + gap), kCardTop * u + row * (card_h + gap), card_w, card_h);
      if (box.top() > height()) break;
      DrawCard(painter, box, collections_[i], int(i) == collection_);
    }
    return;
  }

  // A tag's games as a cover grid, like the search results.
  const int count = int(current.items.size());
  painter.drawText(QPointF(kMargin * u, (kTop + 1.6) * u), QString("%1 (%2)").arg(current.tag).arg(count));
  // Above the hints at the bottom.
  const QRectF area(kMargin * u, (kTop + 3.6) * u, width() - kMargin * u * 2, height() - (kTop + 3.6 + 4.5) * u);
  const CoverGrid grid(area, kResultColumns, u * 1.3, game_ / kResultColumns);
  painter.setClipRect(QRectF(0, area.top() - u * 0.5, width(), height()));
  for (int i = 0; i < count; ++i) {
    const QRectF box = grid.Box(i);
    if (box.bottom() < area.top()) continue;
    if (box.top() > height()) break;
    const Item& item = current.items[size_t(i)];
    DrawCover(painter, box, window_->Cover(item, grid.Tile()), u, i == game_, !item.installed(), window_->Progress(item));
  }
}

}  // namespace mira_gui::bigscreen
