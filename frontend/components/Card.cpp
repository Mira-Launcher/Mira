#include "Card.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

#include "../theme/Theme.h"
#include "DividedRows.h"
#include "Fold.h"
#include "Labels.h"
#include "Metrics.h"
#include "Scrolling.h"
#include "SettingRow.h"
#include "ToolTip.h"

namespace mira_gui {

namespace {

bool HasGrip(const QWidget* row) {
  const auto* setting = qobject_cast<const SettingRow*>(row);
  return setting != nullptr && setting->Grip() != nullptr && !setting->Grip()->isHidden();
}

}  // namespace

Card::Card(const QString& title, QWidget* parent) : QFrame(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(1, 1, 1, 1);  // inside its border, so a sunken row never covers it
  layout->setSpacing(0);

  header_ = new QWidget(this);
  header_layout_ = new QHBoxLayout(header_);
  // Its title starts where the rows' labels do (SettingRow's line, past its dot's room).
  header_layout_->setContentsMargins(kLabelInset, 14, kRowPaddingX - 6, 4);
  header_layout_->setSpacing(10);
  title_ = new QLabel(title, header_);
  SetTextStyle(title_, text::kSection);
  header_layout_->addWidget(title_, /*stretch=*/1);
  header_->setVisible(!title.isEmpty());
  layout->addWidget(header_);

  body_ = new DividedRows(&rows_, this);
  body_layout_ = new QVBoxLayout(body_);
  body_layout_->setContentsMargins(0, 0, 0, 0);
  body_layout_->setSpacing(0);
  layout->addWidget(body_);
  layout->addStretch(1);  // a card stretched to its column's bottom keeps its rows at the top
  footer_ = new DividedFooter(this);
  footer_layout_ = new QVBoxLayout(footer_);
  footer_layout_->setContentsMargins(0, 0, 0, 0);
  footer_layout_->setSpacing(0);
  footer_->hide();
  layout->addWidget(footer_);
}

void Card::AddFooterRow(QWidget* row, int at) {
  row->setParent(footer_);
  footer_layout_->insertWidget(at, row);
  footer_->show();
}

void Card::AddRow(QWidget* row) {
  // Before the stretch a scrolling card keeps under its rows.
  body_layout_->insertWidget(static_cast<int>(rows_.size()), row);
  rows_.append(row);
  if (auto* setting = qobject_cast<SettingRow*>(row)) {
    tooltip::AlignLeftWith(setting->Label(), this);
    if (setting->Grip() != nullptr) setting->Grip()->installEventFilter(this);
  }
}

void Card::ClearRows() {
  for (QWidget* row : rows_) {
    body_layout_->removeWidget(row);
    row->hide();
    row->deleteLater();  // one of its buttons may be what asked for the rebuild
  }
  rows_.clear();
  dragging_ = nullptr;
  body_->update();
}

void Card::RemoveRow(QWidget* row) {
  if (!rows_.removeOne(row)) return;
  body_layout_->removeWidget(row);
  row->hide();
  row->deleteLater();
  if (dragging_ == row) dragging_ = nullptr;
  body_->update();
}

void Card::SetScrollable() {
  if (scroll_ != nullptr) return;
  auto* layout = static_cast<QVBoxLayout*>(this->layout());
  scroll_ = new QScrollArea(this);
  SeeThrough(scroll_);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setWidgetResizable(true);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  layout->replaceWidget(body_, scroll_);
  scroll_->setWidget(body_);
  // The scroll takes the card's spare height in place of the stretch under the rows, which moves
  // inside it so a few rows stay their own height at the top.
  delete layout->takeAt(layout->indexOf(scroll_) + 1);
  layout->setStretchFactor(scroll_, 1);
  body_layout_->addStretch(1);
  SetUpScrolling(scroll_);
}

void Card::SetPinnedRow(QWidget* row) {
  auto* layout = static_cast<QVBoxLayout*>(this->layout());
  pinned_ = new QWidget(this);
  auto* pinned_layout = new QVBoxLayout(pinned_);
  pinned_layout->setContentsMargins(0, 0, 0, 0);
  pinned_layout->addWidget(row);
  layout->insertWidget(layout->indexOf(header_) + 1, pinned_);
}

QString Card::Title() const {
  return title_->text();
}

void Card::MoveRow(QWidget* row, int to) {
  const int from = static_cast<int>(rows_.indexOf(row));
  to = std::clamp(to, 0, static_cast<int>(rows_.size()) - 1);
  if (from < 0 || from == to) return;
  rows_.move(from, to);
  body_layout_->removeWidget(row);
  body_layout_->insertWidget(to, row);
  dragged_ = true;
  body_->update();
}

bool Card::HandleGrip(QWidget* grip, QEvent* event) {
  QWidget* row = grip->parentWidget();
  switch (event->type()) {
    case QEvent::MouseButtonPress:
      if (static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) return false;
      dragging_ = row;
      dragged_ = false;
      // Swallowing the press would otherwise skip focus, and Alt+Up/Down needs it.
      grip->setFocus(Qt::MouseFocusReason);
      grip->setCursor(Qt::ClosedHandCursor);
      theme::SetStyleProperty(row, "dragging", "true");
      return true;
    case QEvent::MouseMove: {
      if (dragging_ != row) return false;
      const int y =
          body_->mapFromGlobal(static_cast<QMouseEvent*>(event)->globalPosition().toPoint()).y();
      for (int i = 0; i < rows_.size(); ++i) {
        const QWidget* other = rows_[i];
        if (other->isVisible() && other != row && HasGrip(other) && y >= other->y() &&
            y < other->y() + other->height()) {
          MoveRow(row, i);
          break;
        }
      }
      return true;
    }
    case QEvent::MouseButtonRelease:
      if (dragging_ != row) return false;
      dragging_ = nullptr;
      grip->setCursor(Qt::OpenHandCursor);
      theme::SetStyleProperty(row, "dragging", "false");
      if (dragged_) emit RowsReordered();
      return true;
    case QEvent::KeyPress: {
      auto* key = static_cast<QKeyEvent*>(event);
      if (key->modifiers() != Qt::AltModifier ||
          (key->key() != Qt::Key_Up && key->key() != Qt::Key_Down)) {
        return false;
      }
      dragged_ = false;
      const int step = key->key() == Qt::Key_Up ? -1 : 1;
      const int to = static_cast<int>(rows_.indexOf(row)) + step;
      if (to >= 0 && to < rows_.size() && HasGrip(rows_[to])) MoveRow(row, to);
      if (dragged_) emit RowsReordered();
      grip->setFocus();
      return true;
    }
    default:
      return false;
  }
}

void Card::SetLeading(QWidget* widget) {
  header_layout_->insertWidget(0, widget, 0, Qt::AlignVCenter);
  header_->show();
}

void Card::AddHeaderWidget(QWidget* widget, Qt::Alignment align) {
  header_layout_->insertWidget(TrailingEnd(), widget, 0, align);
  header_->show();
}

void Card::SetHeaderEnd(QWidget* widget) {
  header_layout_->addWidget(widget, 0, Qt::AlignTop);
  header_end_ = widget;
  header_->show();
}

int Card::TrailingEnd() const {
  for (QWidget* end : {static_cast<QWidget*>(chevron_), header_end_}) {
    if (end != nullptr) return header_layout_->indexOf(end);
  }
  return header_layout_->count();
}

void Card::SetTitle(const QString& title) {
  title_->setText(title);
  header_->setVisible(!title.isEmpty());
}

void Card::SetTitleWidget(QWidget* widget) {
  const int index = header_layout_->indexOf(title_);
  header_layout_->insertWidget(index, widget, /*stretch=*/1);
  title_->hide();
  header_layout_->setStretchFactor(title_, 0);
  header_->show();
}

void Card::SetProminentTitle() {
  SetTextStyle(title_, text::kHeading);
  header_layout_->setContentsMargins(kRowPaddingX + 2, 16, 10, 6);
}

void Card::SetResettable(bool resettable) {
  if (reset_ == nullptr) {
    reset_ = new QToolButton(header_);
    reset_->setObjectName("card_reset");
    reset_->setAutoRaise(true);
    reset_->setText("Reset to defaults");
    // No taller than the title, and its space kept while hidden, so the header never moves.
    reset_->setFixedHeight(title_->sizeHint().height());
    KeepSpaceWhenHidden(reset_);
    connect(reset_, &QToolButton::clicked, this, &Card::ResetClicked);
    AddHeaderWidget(reset_);
  }
  reset_->setVisible(resettable);
}

void Card::SetCollapsible(bool collapsed) {
  if (chevron_ == nullptr) {
    chevron_ = fold::NewChevron(
        header_, this, [this] { return Expanded(); }, [this](bool on) { SetExpanded(on); });
    chevron_->setFocusPolicy(Qt::StrongFocus);
    header_layout_->insertWidget(
        header_end_ != nullptr ? header_layout_->indexOf(header_end_) : header_layout_->count(),
        chevron_);
    header_->setCursor(Qt::PointingHandCursor);
    header_->installEventFilter(this);
    header_->show();
  }
  SetExpanded(!collapsed);
}

void Card::SetSummary(const QString& text) {
  if (summary_ == nullptr) {
    summary_ = new QLabel(header_);
    SetTextStyle(summary_, text::kSubtle);
    header_layout_->insertWidget(TrailingEnd(), summary_, 0, Qt::AlignVCenter);
  }
  summary_->setText(text);
}

void Card::SetExpanded(bool expanded) {
  body_->setVisible(expanded);
  if (chevron_ != nullptr) {
    fold::ShowChevron(chevron_, expanded);
    // Folded, the header is the whole card, so it gets the bottom padding.
    header_layout_->setContentsMargins(kLabelInset, 12, kRowPaddingX - 6, expanded ? 4 : 12);
  }
  update();
}

bool Card::Expanded() const {
  return body_->isVisibleTo(this);
}

void Card::resizeEvent(QResizeEvent* event) {
  QFrame::resizeEvent(event);
  RoundBottomRow();  // a list folding or an expander opening resizes the card
}

bool Card::event(QEvent* event) {
  const bool handled = QFrame::event(event);
  // Rows added or shown inside, which may not resize the card, lay it out again.
  if (event->type() == QEvent::LayoutRequest) RoundBottomRow();
  return handled;
}

void Card::RoundBottomRow() {
  static const QStringList kSunken = {"list_row", "list_more", "expander_body", "expander_head"};
  for (QWidget* widget : findChildren<QWidget*>()) {
    if (!kSunken.contains(widget->objectName())) continue;
    const int bottom = widget->mapTo(this, QPoint(0, widget->height())).y();
    const bool last = widget->isVisibleTo(this) && bottom >= height() - 2;  // its border's inside
    theme::SetStyleProperty(widget, "card_bottom", last ? "true" : "false");
  }
}

bool Card::eventFilter(QObject* watched, QEvent* event) {
  if (auto* grip = qobject_cast<QWidget*>(watched);
      grip != nullptr && grip->objectName() == "row_grip") {
    if (HandleGrip(grip, event)) return true;
  }
  if (watched == header_ && fold::FoldClick(header_, event)) {
    SetExpanded(!Expanded());
    return true;
  }
  return QFrame::eventFilter(watched, event);
}

}  // namespace mira_gui
