#include "FilterSortPill.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/TabRow.h"
#include "LibrarySort.h"

namespace mira_gui {
namespace {

struct FilterEntry {
  const char* label;
  const char* key;
  icons::Glyph icon;
};

const FilterEntry kFilters[] = {
    {"All games", "all", icons::Glyph::Filter},
    {"Playing now", "running", icons::Glyph::Play},
    {"Ready", "ready", icons::Glyph::CheckCircle},
    {"Needs install", "needs_install", icons::Glyph::Download},
    {"Setting up", "setting_up", icons::Glyph::Clock},
    {"Broken", "broken", icons::Glyph::Warning},
    {"Missing", "missing", icons::Glyph::CircleX},
    {"Never played", "never", icons::Glyph::Moon},
    // Every entry above excludes a hidden-tagged game; this is the only one
    // that shows them, and only them.
    {"Hidden", "hidden", icons::Glyph::EyeSlash},
    // After Hidden so Ctrl+1…9 keep their filters.
    {"Needs attention", "attention", icons::Glyph::Warning},
    {"Apps", "apps", icons::Glyph::Wrench},
    {"Games", "games", icons::Glyph::Target},
};

// Icon + label + a live count. Transparent background: the list's own
// selection highlight marks the active row.
QWidget* MakeFilterRow(icons::Glyph glyph, const QString& label, QWidget* parent) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(6, 3, 6, 3);
  layout->setSpacing(8);
  auto* icon = new QLabel(row);
  icon->setObjectName("icon");
  icon->setPixmap(icons::For(glyph, theme::Current().text_muted).pixmap(14, 14));
  layout->addWidget(icon);
  auto* text = new QLabel(label, row);
  layout->addWidget(text, /*stretch=*/1);
  auto* count = new QLabel(row);
  count->setObjectName("count");
  count->setProperty("role", "muted");
  layout->addWidget(count);
  return row;
}

}  // namespace

FilterSortPill::FilterSortPill(const std::string& sort_key, bool sort_descending, QWidget* parent)
    : QWidget(parent), sort_key_(sort_key), sort_descending_(sort_descending) {
  setObjectName("filter_sort_button");
  setCursor(Qt::PointingHandCursor);
  setAttribute(Qt::WA_Hover, true);
  setFixedHeight(TabRow::kControlHeight);

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(8, 0, 8, 0);
  layout->setSpacing(6);
  filter_icon_ = new QLabel(this);
  layout->addWidget(filter_icon_);
  filter_label_ = new QLabel(this);
  layout->addWidget(filter_label_, /*stretch=*/1);
  layout->addWidget(MakeDivider(this, Qt::Vertical, 14));
  sort_icon_ = new QLabel(this);
  layout->addWidget(sort_icon_);
  sort_label_ = new QLabel(this);
  sort_label_->setProperty("role", "subtle");
  layout->addWidget(sort_label_);
  chevron_ = new QLabel(this);
  layout->addWidget(chevron_);

  popover_ = BuildPopover();
  ApplyIcons();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this,
          &FilterSortPill::ApplyIcons);
}

FilterSortPill::~FilterSortPill() { delete popover_; }  // parentless; see BuildPopover

QWidget* FilterSortPill::BuildPopover() {
  // No parent: as the pill's child it would sit inside the tab row, whose
  // checked-button style would then override the sort rows'.
  auto* popover = new QWidget(nullptr, Qt::Popup);
  popover->setObjectName("filter_sort_popover");
  auto* layout = new QVBoxLayout(popover);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(2);

  layout->addWidget(MakeGroupHeading(popover, "FILTER"));
  filters_ = new QListWidget(popover);
  filters_->setObjectName("filter_list");
  filters_->setFrameShape(QFrame::NoFrame);
  filters_->setSelectionMode(QAbstractItemView::SingleSelection);
  filters_->setFocusPolicy(Qt::NoFocus);
  filters_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  for (const FilterEntry& entry : kFilters) {
    auto* item = new QListWidgetItem(filters_);
    item->setData(Qt::UserRole, QString(entry.key));
    auto* row = MakeFilterRow(entry.icon, entry.label, filters_);
    item->setSizeHint(row->sizeHint());
    filters_->setItemWidget(item, row);
  }
  filters_->setCurrentRow(0);
  connect(filters_, &QListWidget::currentRowChanged, this, [this] {
    RestyleFilterRows();
    UpdateSummary();
    emit FilterChanged();
  });
  // QListWidget's own sizeHint doesn't grow with its item count: fit
  // exactly the rows it has, rather than an arbitrary scrollable box.
  int filters_height = 2 * filters_->frameWidth();
  for (int row = 0; row < filters_->count(); ++row) filters_height += filters_->sizeHintForRow(row);
  filters_->setFixedHeight(filters_height);
  layout->addWidget(filters_);

  layout->addWidget(MakeDivider(popover, Qt::Horizontal));

  auto* sort_heading_row = new QHBoxLayout();
  sort_heading_row->addWidget(MakeGroupHeading(popover, "SORT"), /*stretch=*/1);
  sort_direction_ = new QToolButton(popover);
  sort_direction_->setAutoRaise(true);
  connect(sort_direction_, &QToolButton::clicked, this, [this] {
    sort_descending_ = !sort_descending_;
    UpdateSummary();
    emit SortChanged();
  });
  sort_heading_row->addWidget(sort_direction_);
  layout->addLayout(sort_heading_row);

  // Full-width rows, same shape as the filter list above (and the sidebar's
  // own nav rows): a segmented row cramped "Last played"/"Playtime" down
  // to unreadable widths.
  auto* sort_group = new QButtonGroup(popover);
  for (const SortOption& option : SortOptions()) {
    auto* button = new QPushButton(option.label, popover);
    button->setFlat(true);
    button->setCheckable(true);
    button->setChecked(option.key == sort_key_);
    const std::string key = option.key;
    connect(button, &QPushButton::clicked, this, [this, key] {
      sort_key_ = key;
      UpdateSummary();
      emit SortChanged();
    });
    sort_group->addButton(button);
    sort_buttons_.append(button);
    layout->addWidget(button);
  }
  return popover;
}

QString FilterSortPill::FilterKey() const {
  const QListWidgetItem* item = filters_->currentItem();
  return item != nullptr ? item->data(Qt::UserRole).toString() : QString("all");
}

QStringList FilterSortPill::FilterKeys() const {
  QStringList keys;
  for (const FilterEntry& entry : kFilters) keys << entry.key;
  return keys;
}

int FilterSortPill::FilterRow(const QString& key) const {
  for (int row = 0; row < filters_->count(); ++row) {
    if (filters_->item(row)->data(Qt::UserRole).toString() == key) return row;
  }
  return -1;
}

void FilterSortPill::SetFilterRow(int row) {
  filters_->setCurrentRow(row);
}

void FilterSortPill::SetCount(const QString& key, int count) {
  const int row = FilterRow(key);
  if (row < 0) return;
  if (auto* label = filters_->itemWidget(filters_->item(row))->findChild<QLabel*>("count")) {
    label->setText(QString::number(count));
  }
}

void FilterSortPill::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
  popover_->setFixedWidth(qMax(240, width()));
  popover_->move(mapToGlobal(QPoint(0, height() + 4)));
  popover_->show();
}

void FilterSortPill::UpdateSummary() {
  const int row = filters_->currentRow();
  filter_label_->setText(row >= 0 && row < static_cast<int>(std::size(kFilters))
                             ? QString(kFilters[row].label)
                             : QString("All games"));

  QString sort_label = "Name";
  for (const SortOption& option : SortOptions()) {
    if (option.key == sort_key_) {
      sort_label = option.label;
      break;
    }
  }
  sort_label_->setText(QString("%1 %2").arg(sort_label, sort_descending_
                                                            ? QString::fromUtf8("\xe2\x86\x93")
                                                            : QString::fromUtf8("\xe2\x86\x91")));
  // The popover's own button shows the current direction too.
  sort_direction_->setArrowType(sort_descending_ ? Qt::DownArrow : Qt::UpArrow);
  sort_direction_->setToolTip(sort_descending_ ? "Descending. Click for ascending."
                                               : "Ascending. Click for descending.");
}

// setItemWidget bypasses QSS's ::item:selected, so the rows restyle by hand
// (icon included): on_accent when active, else muted.
void FilterSortPill::RestyleFilterRows() {
  for (int row = 0; row < filters_->count(); ++row) {
    QWidget* row_widget = filters_->itemWidget(filters_->item(row));
    const bool current = row == filters_->currentRow();
    const QColor color = current ? theme::Current().on_accent : theme::Current().text_muted;
    for (QLabel* label : row_widget->findChildren<QLabel*>()) {
      if (label->objectName() == "icon") {
        label->setPixmap(icons::For(kFilters[row].icon, color).pixmap(14, 14));
      } else {
        // Count included: on_accent for contrast against the accent
        // background, not left at its ordinary muted gray.
        label->setStyleSheet(current ? QString("color: %1;").arg(color.name()) : QString());
      }
    }
  }
}

void FilterSortPill::ApplyIcons() {
  const QColor muted = theme::Current().text_muted;
  filter_icon_->setPixmap(icons::For(icons::Glyph::Filter, muted).pixmap(14, 14));
  sort_icon_->setPixmap(icons::For(icons::Glyph::SortArrows, muted).pixmap(13, 13));
  chevron_->setPixmap(icons::For(icons::Glyph::ChevronDown, muted).pixmap(12, 12));
  RestyleFilterRows();
  UpdateSummary();
}

}  // namespace mira_gui
