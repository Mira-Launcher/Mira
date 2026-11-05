#include "SettingsNav.h"

#include <QAction>
#include <QButtonGroup>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>

#include "../theme/Theme.h"
#include "../widgets/Scrolling.h"
#include "SettingsCard.h"
#include "SettingsSearch.h"

namespace mira_gui {

QString CategoryNavGroup(const QString& category) {
  static const QStringList kLookAndFeel = {"Interface", "Big screen", "Sidebar", "Shortcuts"};
  static const QStringList kSystem = {"Detection", "Scanning", "Desktop entries",
                                      "Downloads", "Updates",  "Advanced"};
  if (kLookAndFeel.contains(category)) return "Look and feel";
  if (kSystem.contains(category)) return "System";
  return "Games";
}

icons::Glyph CategoryGlyph(const QString& category) {
  using icons::Glyph;
  static const QHash<QString, Glyph> kGlyphs = {
      {"Interface", Glyph::Monitor},     {"Big screen", Glyph::Gamepad},
      {"Sidebar", Glyph::Sidebar},       {"Shortcuts", Glyph::Keyboard},
      {"Library", Glyph::Folder},        {"Metadata", Glyph::Image},
      {"Sources", Glyph::Layers},        {"Runners", Glyph::Wrench},
      {"Launching", Glyph::PlayCircle},  {"Installers", Glyph::Download},
      {"Store launchers", Glyph::Store}, {"Detection", Glyph::Target},
      {"Scanning", Glyph::Refresh},      {"Desktop entries", Glyph::Grid},
      {"Advanced", Glyph::Sliders},      {"Saves", Glyph::Save},
      {"Emulators", Glyph::Chip},        {"Updates", Glyph::Update},
  };
  return kGlyphs.value(category, Glyph::Settings);
}

namespace {

constexpr int kPagePadX = 32;
constexpr int kPagePadBottom = 22;
// Space above a page's title once scrolled to it; the sticky title's top padding matches.
constexpr int kTitleClearance = 14;

template <typename T>
T* AncestorOf(QWidget* widget) {
  for (QWidget* at = widget; at != nullptr; at = at->parentWidget()) {
    if (auto* match = qobject_cast<T*>(at)) return match;
  }
  return nullptr;
}

}  // namespace

SettingsNavWidget::SettingsNavWidget(QWidget* parent) : QWidget(parent) {
  auto* outer = new QHBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  left_ = new QWidget(this);
  left_->setObjectName("settings_nav");
  left_->setAttribute(Qt::WA_StyledBackground);
  left_->setFixedWidth(236);
  // The sidebar's margins, so its rows and these sit in the same column; one more on the
  // right for the border, which beside the sidebar is the splitter's handle instead. The nav's
  // scroll area reaches the edge, its scroll bar in that margin, so the rows keep their width
  // (as wide as the search box) on a window short enough to scroll them.
  constexpr int kRight = 11;
  auto* column = new QVBoxLayout(left_);
  column->setContentsMargins(10, 10, 0, 10);
  column->setSpacing(10);
  left_layout_ = new QVBoxLayout();
  left_layout_->setContentsMargins(0, 0, kRight, 0);
  left_layout_->setSpacing(10);
  column->addLayout(left_layout_);

  search_ = new QLineEdit(left_);
  search_->setObjectName("settings_search");
  search_->setPlaceholderText("Search settings…");
  // Mira's own glyph rather than the desktop's clear icon, which doesn't match the rest.
  auto* clear = search_->addAction(QIcon(), QLineEdit::TrailingPosition);
  icons::Follow(clear, icons::Glyph::Close, &theme::Tokens::text_muted);
  clear->setToolTip("Clear the search");
  clear->setVisible(false);
  connect(clear, &QAction::triggered, search_, &QLineEdit::clear);
  connect(search_, &QLineEdit::textChanged, this, [this, clear](const QString& text) {
    clear->setVisible(!text.isEmpty());
    ApplyFilter();
    jump_->stop();
    jumping_to_ = -1;
    scroll_->verticalScrollBar()->setValue(0);
  });
  left_layout_->addWidget(search_);

  auto* scroll = new QScrollArea(left_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_ = new QWidget(scroll);
  nav_layout_ = new QVBoxLayout(nav_);
  nav_layout_->setSpacing(2);  // the sidebar's row gap
  nav_layout_->addStretch(1);
  scroll->setWidget(nav_);
  column->addWidget(scroll, /*stretch=*/1);
  const auto fit_rows = [this, scroll] {
    const int bar = scroll->verticalScrollBar()->maximum() > 0
                        ? scroll->verticalScrollBar()->sizeHint().width()
                        : 0;
    nav_layout_->setContentsMargins(0, 0, std::max(0, kRight - bar), 0);
  };
  connect(scroll->verticalScrollBar(), &QScrollBar::rangeChanged, this, fit_rows);
  fit_rows();
  outer->addWidget(left_);

  buttons_ = new QButtonGroup(this);
  buttons_->setExclusive(true);
  connect(buttons_, &QButtonGroup::idClicked, this, &SettingsNavWidget::Select);

  scroll_ = new QScrollArea(this);
  scroll_->setObjectName("settings_page");
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  canvas_ = new QWidget(scroll_);
  canvas_->setObjectName("settings_canvas");
  sections_ = new QVBoxLayout(canvas_);
  sections_->setContentsMargins(kPagePadX, kTitleClearance, kPagePadX, kPagePadBottom);
  sections_->setSpacing(36);
  sections_->addStretch(1);
  scroll_->setWidget(canvas_);
  SetUpScrolling(scroll_, this);  // the nav column and search box beside it page the settings too
  canvas_->installEventFilter(this);
  scroll_->viewport()->installEventFilter(this);
  connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this,
          &SettingsNavWidget::SyncToScroll);

  sticky_ = new QWidget(scroll_->viewport());
  sticky_->setObjectName("settings_sticky");
  sticky_->setAttribute(Qt::WA_StyledBackground);
  auto* sticky_layout = new QVBoxLayout(sticky_);
  // Top matches kTitleClearance, so the title doesn't move as it sticks.
  sticky_layout->setContentsMargins(0, kTitleClearance, 0, 20);
  sticky_layout->setSpacing(kTitleGap);
  sticky_group_ = new QLabel(sticky_);
  sticky_group_->setProperty("role", "nav_heading");
  sticky_title_ = new QLabel(sticky_);
  sticky_title_->setProperty("role", "heading");
  sticky_layout->addWidget(sticky_group_);
  sticky_layout->addWidget(sticky_title_);
  sticky_->hide();

  jump_ = new QVariantAnimation(this);
  jump_->setDuration(260);
  jump_->setEasingCurve(QEasingCurve::OutCubic);
  connect(jump_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant& value) { scroll_->verticalScrollBar()->setValue(value.toInt()); });
  connect(jump_, &QVariantAnimation::finished, this, [this] {
    jumping_to_ = -1;
    SyncToScroll();
  });

  empty_state_ = new QLabel("No settings match your search.", this);
  empty_state_->setAlignment(Qt::AlignCenter);
  empty_state_->setProperty("role", "muted");

  content_stack_ = new QStackedWidget(this);
  content_stack_->addWidget(scroll_);
  content_stack_->addWidget(empty_state_);
  outer->addWidget(content_stack_, /*stretch=*/1);

  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this,
          &SettingsNavWidget::RefreshIcons);
}

bool SettingsNavWidget::eventFilter(QObject* watched, QEvent* event) {
  if ((watched == canvas_ || watched == scroll_->viewport()) && event->type() == QEvent::Resize)
    SyncToScroll();
  return QWidget::eventFilter(watched, event);
}

void SettingsNavWidget::RearrangePages() {
  for (const Category& category : categories_) category.page->Rearrange();
}

void SettingsNavWidget::SetBottomRoom(int height) {
  bottom_room_ = height;
  SyncToScroll();
}

// The last page whose top has reached where a jump puts it.
int SettingsNavWidget::CurrentIndex() const {
  const int line = scroll_->verticalScrollBar()->value() + kTitleClearance;
  int current = -1;
  for (size_t i = 0; i < categories_.size(); ++i) {
    const Category& category = categories_[i];
    if (category.page->isHidden()) continue;
    if (current < 0 || category.page->y() <= line) current = static_cast<int>(i);
  }
  return current;
}

void SettingsNavWidget::SyncToScroll() {
  const int value = scroll_->verticalScrollBar()->value();
  const Category* last = nullptr;
  for (const Category& category : categories_) {
    if (!category.page->isHidden()) last = &category;
  }

  // Enough room under the last page for a jump to bring its title to the top.
  if (last != nullptr) {
    const int below_top = last->page->height() + kTitleClearance;
    const int bottom =
        std::max(kPagePadBottom + bottom_room_, scroll_->viewport()->height() - below_top);
    if (sections_->contentsMargins().bottom() != bottom) {
      sections_->setContentsMargins(kPagePadX, kTitleClearance, kPagePadX, bottom);
    }
  }

  // The heading and title of the page the view is in, once its own have started under the
  // top, pushed up by the next page as that one arrives.
  const Category* in = nullptr;
  const Category* next = nullptr;
  for (const Category& category : categories_) {
    if (category.page->isHidden()) continue;
    if (category.page->y() - kTitleClearance < value) {
      in = &category;
    } else if (next == nullptr && in != nullptr) {
      next = &category;
    }
  }
  if (in == nullptr) {
    sticky_->hide();
  } else {
    sticky_title_->setText(in->page->Title());
    sticky_group_->setText(in->group_heading != nullptr ? in->group_heading->text() : QString());
    sticky_group_->setVisible(!sticky_group_->text().isEmpty());
    // Over the page's own column, at its width.
    const int left =
        in->page->mapTo(scroll_->viewport(), QPoint(0, 0)).x() + in->page->ColumnLeft();
    sticky_->resize(SettingsColumnWidth(in->page->width()), sticky_->sizeHint().height());
    int y = 0;
    if (next != nullptr)
      y = std::min(0, next->page->y() - kTitleClearance - value - sticky_->height());
    sticky_->move(left, y);
    // Its line marks the cards scrolling under it; while pushed off, it would float on its own.
    theme::SetStyleProperty(sticky_, "pushed", y < 0 ? "true" : "false");
    sticky_->setVisible(y > -sticky_->height());
    sticky_->raise();
  }

  const int current = jumping_to_ >= 0 ? jumping_to_ : CurrentIndex();
  if (current >= 0 && !categories_[current].button->isChecked())
    categories_[current].button->setChecked(true);
}

void SettingsNavWidget::SetNavWidth(int width) {
  left_->setFixedWidth(width);
}

QWidget* SettingsNavWidget::ContentArea() const {
  return content_stack_;
}

SettingsPage* SettingsNavWidget::AddCategory(const QString& title, icons::Glyph glyph,
                                             const QString& nav_group) {
  if (nav_group != current_group_) {
    current_group_ = nav_group;
    current_heading_ = nullptr;
    if (!nav_group.isEmpty()) {
      current_heading_ = new QLabel(nav_group.toUpper(), nav_);
      current_heading_->setProperty("role", "nav_heading");
      nav_layout_->insertWidget(nav_layout_->count() - 1, current_heading_);
    }
  }

  auto* page = new SettingsPage(title, canvas_);
  // On every page, so a page reads the same reached by a click or a scroll, as the sticky shows it.
  if (current_heading_ != nullptr) page->SetGroupHeading(current_group_);
  sections_->insertWidget(sections_->count() - 1, page);

  auto* button = new QPushButton(title, nav_);
  button->setCheckable(true);
  button->setCursor(Qt::PointingHandCursor);
  auto* count = new QLabel(button);
  count->setAttribute(Qt::WA_TransparentForMouseEvents);
  auto* count_layout = new QHBoxLayout(button);
  count_layout->setContentsMargins(0, 0, 10, 0);
  count_layout->addStretch(1);
  count_layout->addWidget(count);
  nav_layout_->insertWidget(nav_layout_->count() - 1, button);

  Category category;
  category.button = button;
  category.count = count;
  category.group_heading = current_heading_;
  category.page = page;
  category.glyph = glyph;
  categories_.push_back(category);
  const int index = static_cast<int>(categories_.size()) - 1;
  buttons_->addButton(button, index);
  connect(button, &QPushButton::toggled, this, [this] { RefreshIcons(); });

  if (index == 0) button->setChecked(true);
  RefreshIcons();
  return page;
}

void SettingsNavWidget::RefreshIcons() {
  const theme::Tokens& tokens = theme::Current();
  for (const Category& category : categories_) {
    const QColor color = category.button->isChecked() ? tokens.on_accent : tokens.text_muted;
    category.button->setIcon(icons::For(category.glyph, color));
    theme::SetStyleSheet(category.count, QString("color: %1;").arg(color.name()));
  }
}

void SettingsNavWidget::ScrollToTop() {
  jump_->stop();
  canvas_->layout()->activate();
  scroll_->verticalScrollBar()->setValue(0);
  SyncToScroll();
}

void SettingsNavWidget::Select(int index) {
  if (index < 0 || index >= static_cast<int>(categories_.size())) return;
  const Category& category = categories_[index];
  if (category.page->isHidden()) return;
  canvas_->layout()->activate();
  QScrollBar* bar = scroll_->verticalScrollBar();
  const int target = std::clamp(category.page->y() - kTitleClearance, 0, bar->maximum());
  jumping_to_ = index;
  category.button->setChecked(true);
  jump_->stop();
  jump_->setStartValue(bar->value());
  jump_->setEndValue(target);
  jump_->start();
}

void SettingsNavWidget::RegisterRow(QWidget* row_widget, const QString& searchable_text) {
  auto* page = AncestorOf<SettingsPage>(row_widget);
  const auto category = std::ranges::find(categories_, page, &Category::page);
  if (category == categories_.end()) return;  // not on one of these pages

  RowEntry row;
  row.row_widget = row_widget;
  row.card = AncestorOf<SettingsCard>(row_widget);
  row.expander = AncestorOf<ExpanderRow>(row_widget);
  row.search_text = settings_search::Normalize(searchable_text);
  row.category_index = static_cast<int>(category - categories_.begin());
  rows_.push_back(row);
  row_index_by_widget_.insert(row_widget, static_cast<int>(rows_.size()) - 1);
  category->total_rows++;
  // A row rebuilt later (SettingsCard::ClearRows) leaves the search with it.
  connect(row_widget, &QObject::destroyed, this, [this, row_widget] { UnregisterRow(row_widget); });
}

SettingsNavWidget::~SettingsNavWidget() {
  for (const RowEntry& row : rows_) disconnect(row.row_widget, nullptr, this, nullptr);
}

void SettingsNavWidget::UnregisterRow(QWidget* row_widget) {
  const auto it = row_index_by_widget_.constFind(row_widget);
  if (it == row_index_by_widget_.constEnd()) return;
  categories_[size_t(rows_[size_t(it.value())].category_index)].total_rows--;
  rows_.erase(rows_.begin() + it.value());
  row_index_by_widget_.clear();
  for (int i = 0; i < int(rows_.size()); ++i)
    row_index_by_widget_.insert(rows_[size_t(i)].row_widget, i);
}

void SettingsNavWidget::SetRowGateVisible(QWidget* row_widget, bool visible) {
  const auto it = row_index_by_widget_.constFind(row_widget);
  if (it == row_index_by_widget_.constEnd()) return;
  rows_[it.value()].gate_visible = visible;
  ApplyFilter();
}

void SettingsNavWidget::ApplyFilter() {
  const QString query = search_->text().trimmed();
  const bool searching = !query.isEmpty();
  std::vector<int> visible_count(categories_.size(), 0);
  QHash<SettingsCard*, int> card_visible;
  QHash<ExpanderRow*, int> expander_visible;

  for (const RowEntry& row : rows_) {
    const bool visible = row.gate_visible && settings_search::Matches(row.search_text, query);
    row.row_widget->setVisible(visible);
    if (row.card != nullptr) card_visible[row.card] += visible ? 1 : 0;
    if (row.expander != nullptr) expander_visible[row.expander] += visible ? 1 : 0;
    if (visible) visible_count[row.category_index]++;
  }

  // Likewise an expander row: searching opens one with a match.
  for (auto it = expander_visible.cbegin(); it != expander_visible.cend(); ++it) {
    ExpanderRow* expander = it.key();
    expander->setVisible(it.value() > 0);
    if (searching && it.value() > 0) {
      if (!closed_before_search_.contains(expander))
        closed_before_search_.insert(expander, !expander->Expanded());
      expander->SetExpanded(true);
    } else if (!searching && closed_before_search_.contains(expander)) {
      expander->SetExpanded(!closed_before_search_.take(expander));
    }
  }

  // A card shows while any of its rows does. Searching unfolds a folded card
  // with a match, and clearing the search folds it again.
  for (const Category& category : categories_) {
    for (SettingsCard* card : category.page->findChildren<SettingsCard*>()) {
      if (!card_visible.contains(card)) {
        card->setVisible(!searching);  // no searchable rows: only shown outside a search
        continue;
      }
      card->setVisible(card_visible.value(card) > 0);
      if (!card->Collapsible()) continue;
      if (searching && card_visible.value(card) > 0) {
        if (!folded_before_search_.contains(card))
          folded_before_search_.insert(card, !card->Expanded());
        card->SetExpanded(true);
      } else if (!searching && folded_before_search_.contains(card)) {
        card->SetExpanded(!folded_before_search_.take(card));
      }
    }
  }

  // A page with no match hides from the scroll and dims in the nav, with its
  // match count beside the rest. One the gate empties (nothing overridable) hides from both.
  bool any_visible = false;
  QHash<QLabel*, bool> heading_visible;
  for (size_t i = 0; i < categories_.size(); ++i) {
    const Category& category = categories_[i];
    const bool gated_out =
        category.total_rows > 0 && std::ranges::none_of(rows_, [&](const RowEntry& row) {
          return row.category_index == static_cast<int>(i) && row.gate_visible;
        });
    const bool show = category.total_rows == 0 ? !searching : visible_count[i] > 0;
    category.page->setVisible(show);
    category.button->setVisible(!gated_out);
    category.button->setEnabled(show);
    category.count->setText(searching && show && category.total_rows > 0
                                ? QString::number(visible_count[i])
                                : QString());
    if (category.group_heading != nullptr) heading_visible[category.group_heading] |= !gated_out;
    any_visible = any_visible || show;
  }
  for (auto it = heading_visible.cbegin(); it != heading_visible.cend(); ++it)
    it.key()->setVisible(it.value());

  content_stack_->setCurrentWidget(searching && !any_visible ? static_cast<QWidget*>(empty_state_)
                                                             : scroll_);
  canvas_->layout()->activate();
  SyncToScroll();
}

void SettingsNavWidget::RevealRow(QWidget* row_widget) {
  const auto it = row_index_by_widget_.constFind(row_widget);
  if (it == row_index_by_widget_.constEnd()) return;
  const RowEntry& row = rows_[it.value()];

  if (!search_->text().isEmpty()) search_->clear();  // triggers ApplyFilter via textChanged

  if (row.card != nullptr && row.card->Collapsible()) row.card->SetExpanded(true);
  if (row.expander != nullptr) row.expander->SetExpanded(true);
  // After the unfolded card has its size, at the top just clear of the sticky title: scrolling
  // only until it's in view leaves it at the bottom, under the rows above it.
  QTimer::singleShot(0, this, [this, row_widget = QPointer<QWidget>(row_widget)] {
    if (row_widget == nullptr) return;
    SettleLayout(canvas_);
    const int top = row_widget->mapTo(canvas_, QPoint(0, 0)).y();
    scroll_->verticalScrollBar()->setValue(top - sticky_->sizeHint().height() - 4);
  });
}

void SettingsNavWidget::RevealPage(SettingsPage* page) {
  if (!search_->text().isEmpty()) search_->clear();
  const auto it = std::ranges::find(categories_, page, &Category::page);
  if (it == categories_.end()) return;
  const int index = static_cast<int>(it - categories_.begin());
  QTimer::singleShot(0, this, [this, index] { Select(index); });
}

void SettingsNavWidget::SetHeaderWidget(QWidget* widget) {
  if (header_widget_ != nullptr) {
    left_layout_->removeWidget(header_widget_);
    header_widget_->deleteLater();
  }
  header_widget_ = widget;
  left_layout_->insertWidget(0, widget);
}

std::vector<CategoryRows> GroupByCategory(const std::vector<std::string>& categories) {
  std::vector<CategoryRows> out;
  for (size_t i = 0; i < categories.size(); ++i) {
    const QString name =
        categories[i].empty() ? QString("General") : QString::fromStdString(categories[i]);
    auto it = std::ranges::find(out, name, &CategoryRows::name);
    if (it == out.end()) it = out.insert(out.end(), CategoryRows{name, {}});
    it->rows.push_back(i);
  }
  return out;
}

}  // namespace mira_gui
