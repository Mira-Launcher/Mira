#include "TabRow.h"

#include <algorithm>

#include <QEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include "Icons.h"

namespace mira_gui {
namespace {

constexpr int kSearchMax = 320;
constexpr int kSearchMin = 160;
constexpr int kStretchMin = 12;  // gap kept between the tabs and the actions

int WidthOf(const QWidget* widget) {
  return widget->minimumWidth() == widget->maximumWidth() ? widget->minimumWidth() : widget->sizeHint().width();
}

}  // namespace

TabRow::TabRow(QWidget* parent) : QWidget(parent) {
  setObjectName("tab_row");
  setAttribute(Qt::WA_StyledBackground, true);
  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(4);
  tabs_ = new QHBoxLayout();
  tabs_->setSpacing(4);
  layout_->addLayout(tabs_);
  layout_->addStretch(1);

  more_ = new QPushButton("More", this);
  more_->setCursor(Qt::PointingHandCursor);
  more_menu_ = new QMenu(more_);
  connect(more_menu_, &QMenu::aboutToShow, this, [this] {
    more_menu_->clear();
    for (const QString& key : order_) {
      const Tab& tab = by_key_[key];
      if (!tab.button->isHidden()) continue;
      connect(more_menu_->addAction(tab.button->text()), &QAction::triggered, this, [this, key] { Select(key); });
    }
  });
  more_->setMenu(more_menu_);
  more_->hide();
  tabs_->addWidget(more_);
}

void TabRow::AddTab(const QString& key, const QString& label) {
  auto* button = new QPushButton(this);
  button->setCheckable(true);
  button->setFlat(true);
  button->setCursor(Qt::PointingHandCursor);
  connect(button, &QPushButton::clicked, this, [this, key] { Select(key); });
  tabs_->insertWidget(tabs_->indexOf(more_), button);
  by_key_.insert(key, Tab{button, label});
  order_.append(key);
  Relabel(by_key_[key]);
}

void TabRow::Select(const QString& key) {
  // Clicking the current tab again keeps it current.
  const bool changed = key != current_;
  SetCurrent(key);
  if (changed) emit CurrentChanged(key);
}

void TabRow::Relabel(Tab& tab) {
  tab.button->setText(tab.count < 0 ? tab.label : QString("%1  %2").arg(tab.label).arg(tab.count));
  tab.button->setProperty("alert", tab.alert && tab.count > 0);
  tab.button->style()->unpolish(tab.button);
  tab.button->style()->polish(tab.button);
  Fit();
}

void TabRow::SetCount(const QString& key, int count) {
  const auto it = by_key_.find(key);
  if (it == by_key_.end() || it->count == count) return;
  it->count = count;
  Relabel(*it);
}

void TabRow::SetAlert(const QString& key, bool alert) {
  const auto it = by_key_.find(key);
  if (it == by_key_.end()) return;
  it->alert = alert;
  Relabel(*it);
}

void TabRow::SetCurrent(const QString& key) {
  current_ = by_key_.contains(key) ? key : QString();
  for (auto it = by_key_.begin(); it != by_key_.end(); ++it) it->button->setChecked(it.key() == current_);
  Fit();  // the current tab is bold, so wider, and always stays in the row
}

QString TabRow::Current() const { return current_; }

void TabRow::SetTabsVisible(bool visible) {
  tabs_visible_ = visible;
  Fit();
}

void TabRow::SetTrailing(QWidget* widget) {
  layout_->insertWidget(search_button_ != nullptr ? layout_->indexOf(search_button_) : layout_->count(), widget);
  trailing_.append(widget);
  Fit();
}

void TabRow::SetSearch(QLineEdit* search) {
  search_ = search;
  search_->setFixedHeight(kControlHeight);
  search_->installEventFilter(this);
  search_button_ = new QToolButton(this);
  search_button_->setIcon(icons::For(icons::Glyph::Search));
  search_button_->setToolTip("Search");
  search_button_->setAutoRaise(true);
  search_button_->setFixedSize(kControlHeight, kControlHeight);
  search_button_->hide();
  connect(search_button_, &QToolButton::clicked, this, [this] {
    OpenSearch();
    search_->setFocus(Qt::OtherFocusReason);
  });
  layout_->addWidget(search_button_);
  layout_->addWidget(search_);
  Fit();
}

void TabRow::OpenSearch() {
  search_open_ = true;
  Fit();
}

bool TabRow::event(QEvent* event) {
  // A trailing action shown or hidden, or its text changed.
  if (event->type() == QEvent::LayoutRequest) Fit();
  return QWidget::event(event);
}

bool TabRow::eventFilter(QObject* watched, QEvent* event) {
  if (watched == search_ && event->type() == QEvent::FocusOut && search_open_) {
    // Back to its button once left empty; after the focus change settles.
    QTimer::singleShot(0, this, [this] {
      if (search_->hasFocus() || !search_->text().isEmpty()) return;
      search_open_ = false;
      Fit();
    });
  }
  return QWidget::eventFilter(watched, event);
}

void TabRow::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  Fit();
}

void TabRow::Fit() {
  if (fitting_) return;
  fitting_ = true;
  const int spacing = layout_->spacing();
  const int room = width() - kStretchMin;
  int trailing = 0;
  for (const QWidget* widget : trailing_) {
    if (!widget->isHidden()) trailing += WidthOf(widget) + spacing;
  }

  auto tabs_width = [&](const QStringList& keys, bool more) {
    int total = 0;
    for (const QString& key : keys) total += by_key_[key].button->sizeHint().width() + spacing;
    return total + (more ? more_->sizeHint().width() + spacing : 0);
  };

  // The search box first: as wide as the room left beside every tab, within
  // 160 to 320 px; below that a button, unless it's open or holds a query.
  int search_space = 0;
  if (search_ != nullptr) {
    const int beside = room - trailing - (tabs_visible_ ? tabs_width(order_, false) : 0) - spacing;
    bool collapsed = false;
    int search_width = kSearchMin;
    if (beside >= kSearchMin) {
      search_width = std::min(kSearchMax, beside);
    } else if (!search_open_ && search_->text().isEmpty() && !search_->hasFocus()) {
      collapsed = true;
    }
    search_->setVisible(!collapsed);
    search_button_->setVisible(collapsed);
    if (!collapsed) search_->setFixedWidth(search_width);
    search_space = (collapsed ? kControlHeight : search_width) + spacing;
  }

  // Then the tabs: as many as fit in order, keeping the current one, the
  // rest in More.
  QStringList shown = tabs_visible_ ? order_ : QStringList();
  if (tabs_visible_) {
    const int budget = room - trailing - search_space;
    for (qsizetype keep = order_.size(); keep >= 1; --keep) {
      shown = order_.mid(0, keep);
      if (!current_.isEmpty() && !shown.contains(current_)) {
        shown.removeLast();
        shown.append(current_);
      }
      if (tabs_width(shown, shown.size() < order_.size()) <= budget) break;
    }
  }
  for (const QString& key : order_) by_key_[key].button->setVisible(shown.contains(key));
  more_->setVisible(tabs_visible_ && shown.size() < order_.size());
  fitting_ = false;
}

QString StatusDot(const QColor& color) {
  return QString("<span style='color:%1'>●</span> ").arg(color.name());
}

}  // namespace mira_gui
