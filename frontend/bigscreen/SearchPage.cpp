#include "SearchPage.h"

#include <QPainter>

#include <algorithm>

#include "../library/ArtworkStore.h"
#include "../library/GameLibraryModel.h"
#include "../library/OwnedTitles.h"
#include "../theme/Theme.h"
#include "BigScreenWindow.h"

namespace mira_gui::bigscreen {
namespace {

const QString kKeys = "abcdefghijklmnopqrstuvwxyz0123456789";
constexpr int kKeyColumns = 6;
constexpr int kResultColumns = 5;
constexpr double kMargin = 2.6, kTop = 6.4, kKeysW = 24;
constexpr int kRecentSearches = 5;

// Whether one of the game's tags, not a meaning tag, contains `query`.
bool TagMatches(const Item& item, const QString& query) {
  if (!item.game) return false;
  return std::ranges::any_of(item.game->tags, [&](const std::string& tag) {
    return !IsMeaningTag(tag) && QString::fromStdString(tag).contains(query, Qt::CaseInsensitive);
  });
}

}  // namespace

SearchPage::SearchPage(BigScreenWindow* window) : Page(window) {
  const LibraryServices& services = window->services();
  connect(services.library, &GameLibraryModel::Changed, this, &SearchPage::Search);
  connect(services.owned_titles, &OwnedTitles::Changed, this, &SearchPage::Search);
  connect(services.artwork, &ArtworkStore::CoverChanged, this, qOverload<>(&QWidget::update));
  connect(services.artwork, &ArtworkStore::RequestsDropped, this, qOverload<>(&QWidget::update));
}

void SearchPage::Shown() {
  // Arrive on the search box, not typing, so Q/E still switch tabs.
  zone_ = Zone::Field;
  Search();
}

void SearchPage::Search() {
  const LibraryServices& services = window_->services();
  std::vector<Item> all;
  QStringList sources;
  for (const GameSummary& game : services.library->Games()) {
    if (!window_->Browsable(game)) continue;
    all.push_back(window_->ItemFor(game));
  }
  for (const StoreTitle& title : services.owned_titles->Titles()) {
    if (title.owned && !title.installed && window_->Browsable(title)) all.push_back(window_->ItemFor(title));
  }
  for (const Item& item : all) {
    if (!sources.contains(item.source)) sources << item.source;
  }
  std::ranges::sort(sources);
  const QString chosen = filters_.value(filter_, "All");
  filters_ = QStringList{"All", "Installed"} + sources;
  filter_ = std::max(0, int(filters_.indexOf(chosen)));

  results_.clear();
  const QString query = query_.simplified();
  for (Item& item : all) {
    if (!query.isEmpty() && !item.name.contains(query, Qt::CaseInsensitive) && !TagMatches(item, query)) continue;
    if (chosen == "Installed" && !item.installed()) continue;
    if (chosen != "All" && chosen != "Installed" && item.source != chosen) continue;
    results_.push_back(std::move(item));
  }
  std::ranges::sort(results_, [](const Item& a, const Item& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
  result_ = std::clamp(result_, 0, std::max(0, Entries() - 1));
  if (zone_ == Zone::Results && Entries() == 0) zone_ = Zone::Keys;
  if (zone_ == Zone::Results && !ShowRecents()) window_->ShowHero(results_[size_t(result_)]);
  update();
  emit HintsChanged();
}

QStringList SearchPage::Recents() const {
  const std::vector<std::string> saved = window_->prefs().big_screen_recent_searches.value_or(std::vector<std::string>{});
  QStringList recents;
  for (const std::string& text : saved) {
    if (recents.size() < kRecentSearches) recents << QString::fromStdString(text);
  }
  return recents;
}

bool SearchPage::ShowRecents() const { return query_.simplified().isEmpty() && !Recents().isEmpty(); }

int SearchPage::Entries() const { return ShowRecents() ? int(Recents().size()) : int(results_.size()); }

void SearchPage::SaveRecent() {
  const QString query = query_.simplified();
  if (query.isEmpty()) return;
  FrontendPrefs prefs = window_->prefs();
  std::vector<std::string> recent = prefs.big_screen_recent_searches.value_or(std::vector<std::string>{});
  std::erase_if(recent, [&](const std::string& text) { return QString::fromStdString(text).compare(query, Qt::CaseInsensitive) == 0; });
  recent.insert(recent.begin(), query.toStdString());
  if (int(recent.size()) > kRecentSearches) recent.resize(size_t(kRecentSearches));
  prefs.big_screen_recent_searches = recent;
  window_->SetPrefs(prefs);
}

bool SearchPage::Typed(const QString& text) {
  // Only the keyboard zone takes typing; elsewhere letters are buttons (Q/E switch tabs).
  if (zone_ != Zone::Keys) return false;
  if (text.isEmpty()) {
    if (query_.isEmpty()) return false;
    query_.chop(1);
  } else {
    query_ += text.toLower();
  }
  result_ = 0;
  Search();
  return true;
}

bool SearchPage::Navigate(Nav nav) {
  const int results = Entries();
  if (nav == Nav::Action && zone_ == Zone::Keys) {
    query_.chop(1);
    Search();
    return true;
  }
  if (nav == Nav::Search) {
    if (!query_.isEmpty() && !query_.endsWith(' ')) query_ += ' ';
    Search();
    return true;
  }
  switch (zone_) {
    case Zone::Field:
      if (nav == Nav::Down || nav == Nav::Accept) zone_ = Zone::Keys;
      if (nav == Nav::Right) zone_ = Zone::Filters;
      break;
    case Zone::Keys: {
      const int row = key_ / kKeyColumns, col = key_ % kKeyColumns;
      if (nav == Nav::Accept) {
        query_ += kKeys[key_];
        result_ = 0;
        Search();
        return true;
      }
      if (nav == Nav::Left && col > 0) --key_;
      if (nav == Nav::Right && col < kKeyColumns - 1) ++key_;
      if (nav == Nav::Right && col == kKeyColumns - 1 && results > 0) zone_ = Zone::Results, result_ = 0;
      if (nav == Nav::Up) row > 0 ? void(key_ -= kKeyColumns) : void(zone_ = Zone::Field);
      if (nav == Nav::Down && key_ + kKeyColumns < int(kKeys.size())) key_ += kKeyColumns;
      break;
    }
    case Zone::Filters:
      if (nav == Nav::Left && filter_ == 0) {
        zone_ = Zone::Field;
        break;
      }
      if (nav == Nav::Left) filter_ = std::max(0, filter_ - 1);
      if (nav == Nav::Right) filter_ = std::min(int(filters_.size()) - 1, filter_ + 1);
      if (nav == Nav::Down) zone_ = results > 0 ? Zone::Results : Zone::Keys;
      if (nav == Nav::Left || nav == Nav::Right || nav == Nav::Accept) {
        result_ = 0;
        Search();
        return true;
      }
      break;
    case Zone::Results: {
      if (ShowRecents()) {
        if (nav == Nav::Accept) {
          query_ = Recents()[result_];
          result_ = 0;
          Search();
          return true;
        }
        if (nav == Nav::Left) result_ > 0 ? void(--result_) : void((zone_ = Zone::Keys, key_ = kKeyColumns - 1));
        if (nav == Nav::Right && result_ + 1 < results) ++result_;
        if (nav == Nav::Up) zone_ = Zone::Filters;
        break;
      }
      const int col = result_ % kResultColumns;
      if (nav == Nav::Accept) {
        SaveRecent();
        window_->OpenGame(results_[size_t(result_)]);
        return true;
      }
      if (nav == Nav::Action) {
        SaveRecent();
        window_->QuickAction(results_[size_t(result_)]);
        return true;
      }
      if (nav == Nav::Left) col > 0 ? void(--result_) : void((zone_ = Zone::Keys, key_ = kKeyColumns - 1));
      if (nav == Nav::Right && col < kResultColumns - 1 && result_ + 1 < results) ++result_;
      if (nav == Nav::Up) result_ >= kResultColumns ? void(result_ -= kResultColumns) : void(zone_ = Zone::Filters);
      if (nav == Nav::Down && result_ + kResultColumns < results) result_ += kResultColumns;
      break;
    }
  }
  if (nav == Nav::Back || nav == Nav::PrevTab || nav == Nav::NextTab) return false;
  if (zone_ == Zone::Results && !ShowRecents()) window_->ShowHero(results_[size_t(result_)]);
  update();
  emit HintsChanged();
  return true;
}

QList<Hint> SearchPage::Hints() const {
  switch (zone_) {
    case Zone::Field: return {{Nav::Accept, "Type"}, {Nav::Back, "Back"}};
    case Zone::Keys:
      return {{Nav::Accept, "Type"}, {Nav::Action, "Delete"}, {Nav::Search, "Space"}, {Nav::Back, "Back"}};
    case Zone::Filters: return {{Nav::Accept, "Filter"}, {Nav::Back, "Back"}};
    case Zone::Results:
      if (ShowRecents()) return {{Nav::Accept, "Search"}, {Nav::Back, "Back"}};
      return {{Nav::Accept, "Details"}, {Nav::Back, "Back"}};
  }
  return {};
}

void SearchPage::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::TextAntialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = window_->unit();
  painter.fillRect(rect(), QColor(tokens.window.red(), tokens.window.green(), tokens.window.blue(), 150));

  // Query field and keyboard.
  const QRectF field(kMargin * u, kTop * u, kKeysW * u, 3.4 * u);
  painter.setPen(zone_ == Zone::Field ? QPen(Accent(), u * 0.12) : QPen(tokens.border, 1));
  painter.setBrush(tokens.surface);
  painter.drawRoundedRect(field, u * 0.5, u * 0.5);
  painter.setFont(Font(u, 1.25));
  painter.setPen(query_.isEmpty() ? tokens.text_muted : tokens.text);
  const QRectF text_box = field.adjusted(u * 1.1, 0, -u * 1.1, 0);
  painter.drawText(text_box, Qt::AlignVCenter | Qt::AlignLeft, query_.isEmpty() ? "Search your games" : query_);
  const double caret_x = text_box.left() + (query_.isEmpty() ? 0 : painter.fontMetrics().horizontalAdvance(query_) + u * 0.1);
  if (zone_ == Zone::Keys) painter.fillRect(QRectF(caret_x, field.center().y() - u * 0.7, u * 0.1, u * 1.4), Accent());

  const double gap = u * 0.45;
  const double key_w = (kKeysW * u - gap * (kKeyColumns - 1)) / kKeyColumns;
  painter.setFont(Font(u, 1.1, QFont::Bold));
  for (int i = 0; i < int(kKeys.size()); ++i) {
    const QRectF key(kMargin * u + (i % kKeyColumns) * (key_w + gap), field.bottom() + u * 1.1 + (i / kKeyColumns) * (u * 3.1 + gap),
                     key_w, u * 3.1);
    const bool focused = zone_ == Zone::Keys && i == key_;
    painter.setPen(Qt::NoPen);
    painter.setBrush(focused ? Accent() : tokens.surface);
    painter.drawRoundedRect(key, u * 0.4, u * 0.4);
    painter.setPen(focused ? tokens.on_accent : tokens.text);
    painter.drawText(key, Qt::AlignCenter, kKeys[i].toUpper());
  }

  // Filters.
  const double right = (kMargin * 2 + kKeysW) * u;
  double x = right;
  painter.setFont(Font(u, 0.95, QFont::DemiBold));
  for (int i = 0; i < filters_.size(); ++i) {
    const QString name = i < 2 ? filters_[i] : SourceName(filters_[i]);
    const double w = painter.fontMetrics().horizontalAdvance(name) + u * 2;
    const QRectF chip(x, kTop * u, w, u * 2.3);
    const bool focused = zone_ == Zone::Filters && i == filter_;
    painter.setPen(focused ? QPen(Accent(), u * 0.12) : Qt::NoPen);
    painter.setBrush(i == filter_ ? tokens.surface_alt : tokens.surface);
    painter.drawRoundedRect(chip, chip.height() / 2, chip.height() / 2);
    painter.setPen(i == filter_ || focused ? tokens.text : tokens.text_muted);
    painter.drawText(chip, Qt::AlignCenter, name);
    x += w + u * 0.5;
  }

  // Results, scrolled so the focused row stays in view.
  const double results_top = (kTop + 3.6) * u;
  painter.setFont(Font(u, 0.9));
  painter.setPen(tokens.text_muted);
  if (ShowRecents()) {
    painter.drawText(QPointF(right, results_top + u), "Recent searches");
    painter.setFont(Font(u, 0.95, QFont::DemiBold));
    double chip_x = right;
    const QStringList recents = Recents();
    for (int i = 0; i < recents.size() && chip_x < width(); ++i) {
      const double w = painter.fontMetrics().horizontalAdvance(recents[i]) + u * 2;
      const QRectF chip(chip_x, results_top + u * 2.4, w, u * 2.3);
      const bool focused = zone_ == Zone::Results && i == result_;
      painter.setPen(focused ? QPen(Accent(), u * 0.12) : Qt::NoPen);
      painter.setBrush(tokens.surface);
      painter.drawRoundedRect(chip, chip.height() / 2, chip.height() / 2);
      painter.setPen(focused ? tokens.text : tokens.text_muted);
      painter.drawText(chip, Qt::AlignCenter, recents[i]);
      chip_x += w + u * 0.5;
    }
    return;
  }
  if (results_.empty()) {
    painter.drawText(QPointF(right, results_top + u), query_.isEmpty() ? "No games here." : "No games match “" + query_ + "”.");
    return;
  }
  painter.drawText(QPointF(right, results_top + u), QString("%1 %2").arg(results_.size()).arg(results_.size() == 1 ? "game" : "games"));
  const QRectF area(right, results_top + u * 2.4, width() - right - kMargin * u, height() - results_top - u * (2.4 + 4.5));
  const CoverGrid grid(area, kResultColumns, u * 1.3, zone_ == Zone::Results ? result_ / kResultColumns : 0);
  painter.setClipRect(QRectF(right - u, results_top + u * 1.6, width(), area.bottom() + u - (results_top + u * 1.6)));
  for (int i = 0; i < int(results_.size()); ++i) {
    const QRectF box = grid.Box(i);
    if (box.bottom() < area.top()) continue;
    if (box.top() > height()) break;
    const bool focused = zone_ == Zone::Results && i == result_;
    DrawCover(painter, box, window_->Cover(results_[size_t(i)], grid.Tile()), u, focused, !results_[size_t(i)].installed(),
              window_->Progress(results_[size_t(i)]));
  }
}

}  // namespace mira_gui::bigscreen
