#include "SettingRow.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "DividedRows.h"
#include "Fold.h"
#include "Labels.h"
#include "Metrics.h"
#include "ToolTip.h"

namespace mira_gui {

// --- SettingRow -----------------------------------------------------------

SettingRow::SettingRow(const QString& label, const QString& doc, QWidget* parent)
    : QWidget(parent) {
  setObjectName("setting_row");
  setAttribute(Qt::WA_StyledBackground);  // so a row being dragged can be highlighted
  outer_ = new QVBoxLayout(this);
  outer_->setContentsMargins(0, 0, 0, 0);
  outer_->setSpacing(0);

  line_ = new QHBoxLayout();
  line_->setContentsMargins(8, 8, kRowPaddingX, 8);
  line_->setSpacing(6);
  outer_->addLayout(line_);

  dot_ = new QWidget(this);
  dot_->setObjectName("row_dot");
  dot_->setFixedSize(6, 6);
  dot_->setToolTip("Not saved yet");
  KeepSpaceWhenHidden(dot_);
  dot_->hide();
  line_->addWidget(dot_, 0, Qt::AlignVCenter);
  line_->addSpacing(4);

  label_ = new QLabel(label, this);
  label_->setMinimumHeight(kControlHeight);
  line_->addWidget(label_);
  if (!doc.isEmpty()) {
    label_->setToolTip(doc);
    label_->setAccessibleDescription(doc);
    doc_delay_ = new QTimer(this);
    doc_delay_->setSingleShot(true);
    doc_delay_->setInterval(300);
    connect(doc_delay_, &QTimer::timeout, this, [this] {
      if (!label_->underMouse()) return;
      QHelpEvent help(QEvent::ToolTip, label_->mapFromGlobal(QCursor::pos()), QCursor::pos());
      QApplication::sendEvent(label_, &help);
    });
    label_->installEventFilter(this);
  }
  label_end_ = line_->count();
  line_->addStretch(1);

  revert_ = new QToolButton(this);
  revert_->setObjectName("row_revert");
  revert_->setAutoRaise(true);
  icons::Follow(revert_, icons::Glyph::Undo, &theme::Tokens::text_muted);
  revert_->setToolTip("Undo this change");
  revert_->setAccessibleName("Undo the change to " + label);
  KeepSpaceWhenHidden(revert_);
  revert_->hide();
  connect(revert_, &QToolButton::clicked, this, &SettingRow::RevertClicked);
  line_->addWidget(revert_, 0, Qt::AlignVCenter);
}

bool SettingRow::eventFilter(QObject* watched, QEvent* event) {
  if (watched == label_) {
    if (event->type() == QEvent::Enter) doc_delay_->start();
    if (event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
      doc_delay_->stop();
    }
  }
  return QWidget::eventFilter(watched, event);
}

void SettingRow::ShowGrip() {
  if (grip_ != nullptr) return;
  auto* grip = new QToolButton(this);
  grip->setObjectName("row_grip");
  grip->setAutoRaise(true);
  icons::Follow(grip, icons::Glyph::Grip, &theme::Tokens::text_muted);
  grip->setCursor(Qt::OpenHandCursor);
  grip->setToolTip("Drag to reorder, or press Alt+Up or Alt+Down");
  grip->setAccessibleName("Reorder " + label_->text());
  line_->insertWidget(line_->indexOf(label_), grip, 0, Qt::AlignVCenter);
  ++label_end_;
  grip_ = grip;
}

void SettingRow::SetGripShown(bool shown) {
  ShowGrip();
  KeepSpaceWhenHidden(grip_);
  grip_->setVisible(shown);
}

void SettingRow::AddControl(QWidget* control, int stretch) {
  line_->addWidget(control, stretch, Qt::AlignVCenter);
}

void SettingRow::AddWideControl(QWidget* control) {
  // Before the revert button, whose kept space would indent it past the other rows' labels.
  line_->insertWidget(label_end_++, control, 1, Qt::AlignVCenter);
  line_->setStretch(label_end_, 0);  // the spacer between the label and the controls
}

void SettingRow::SetLeading(QWidget* widget) {
  line_->insertWidget(line_->indexOf(label_), widget, 0, Qt::AlignVCenter);
  ++label_end_;
}

void SettingRow::AddAfterLabel(QWidget* widget) {
  line_->insertWidget(label_end_++, widget, 0, Qt::AlignVCenter);
}

void SettingRow::SetBelow(QWidget* widget) {
  auto* indent = new QHBoxLayout();
  const QMargins line = line_->contentsMargins();
  indent->setContentsMargins(line.left() + 16, 0, line.right(), line.bottom());
  line_->setContentsMargins(line.left(), line.top(), line.right(), 6);
  indent->addWidget(widget);
  outer_->addLayout(indent);
}

void SettingRow::SetBelowFlush(QWidget* widget) {
  outer_->addWidget(widget);
}

void SettingRow::SetIndent(int pixels) {
  const QMargins line = line_->contentsMargins();
  line_->setContentsMargins(8 + pixels, line.top(), line.right(), line.bottom());
  for (int i = 0; i < outer_->count(); ++i) {
    if (auto* below = qobject_cast<QHBoxLayout*>(outer_->itemAt(i)->layout());
        below != nullptr && below != line_) {
      const QMargins m = below->contentsMargins();
      below->setContentsMargins(8 + pixels + 16, m.top(), m.right(), m.bottom());
    }
  }
}

// --- ExpanderRow ----------------------------------------------------------

ExpanderRow::ExpanderRow(const QString& label, QWidget* parent) : QWidget(parent) {
  setObjectName("expander_row");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  head_ = new QWidget(this);
  head_->setObjectName("expander_head");
  head_->setAttribute(Qt::WA_StyledBackground);
  head_->setAttribute(Qt::WA_Hover);
  head_->setCursor(Qt::PointingHandCursor);
  head_layout_ = new QHBoxLayout(head_);
  // As a SettingRow's line with its dot's room, so names line up with the labels around them.
  head_layout_->setContentsMargins(kLabelInset, 8, kRowPaddingX - 6, 8);
  head_layout_->setSpacing(8);
  label_ = new QLabel(label, head_);
  label_->setMinimumHeight(kControlHeight);
  head_layout_->addWidget(label_);
  meta_ = new QLabel(head_);
  SetTextStyle(meta_, text::kSubtle);
  meta_->hide();
  head_layout_->addWidget(meta_);
  head_layout_->addStretch(1);
  summary_ = new QLabel(head_);
  SetTextStyle(summary_, text::kSubtle);
  summary_->hide();
  head_layout_->addWidget(summary_);
  chevron_ = fold::NewChevron(
      head_, this, [this] { return Expanded(); }, [this](bool on) { SetExpanded(on); });
  chevron_->setAccessibleName(label);
  head_layout_->addWidget(chevron_);
  head_->installEventFilter(this);
  layout->addWidget(head_);

  body_ = new DividedRows(&rows_, this);
  body_->setObjectName("expander_body");
  body_->setAttribute(Qt::WA_StyledBackground);
  body_layout_ = new QVBoxLayout(body_);
  body_layout_->setContentsMargins(0, 1, 0, 0);  // under the line over the body
  body_layout_->setSpacing(0);
  layout->addWidget(body_);
  SetExpanded(false);
}

void ExpanderRow::AddRow(QWidget* row) {
  row->setParent(body_);
  if (auto* setting = qobject_cast<SettingRow*>(row)) setting->SetIndent(16);
  body_layout_->addWidget(row);
  rows_.append(row);
}

void ExpanderRow::ClearRows() {
  for (QWidget* row : rows_) {
    body_layout_->removeWidget(row);
    row->hide();
    row->deleteLater();
  }
  rows_.clear();
}

void ExpanderRow::SetMeta(const QString& text) {
  meta_->setText(text);
  meta_->setVisible(!text.isEmpty());
}

void ExpanderRow::SetSummary(const QString& text, Tone tone) {
  summary_->setText(text);
  summary_->setVisible(!text.isEmpty());
  SetTextStyle(summary_, {TextRole::Body, tone});
}

void ExpanderRow::AddTrailing(QWidget* widget) {
  widget->setParent(head_);
  head_layout_->insertWidget(head_layout_->indexOf(chevron_), widget, 0, Qt::AlignVCenter);
}

void ExpanderRow::SetLeading(QWidget* widget) {
  widget->setParent(head_);
  head_layout_->insertWidget(0, widget, 0, Qt::AlignVCenter);
}

void ExpanderRow::SetExpanded(bool expanded) {
  const bool changed = expanded != Expanded();
  body_->setVisible(expanded);
  fold::ShowChevron(chevron_, expanded);
  if (changed) emit Toggled(expanded);
}

bool ExpanderRow::Expanded() const {
  return body_->isVisibleTo(this);
}

bool ExpanderRow::eventFilter(QObject* watched, QEvent* event) {
  if (watched == head_ && fold::FoldClick(head_, event)) {
    SetExpanded(!Expanded());
    return true;
  }
  return QWidget::eventFilter(watched, event);
}

QWidget* FieldWithButtons(QWidget* field, const std::vector<QWidget*>& buttons) {
  auto* group = new QWidget(field->parentWidget());
  group->setFixedWidth(kFieldColumnWidth);
  auto* layout = new QHBoxLayout(group);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  field->setMinimumWidth(0);
  field->setMaximumWidth(QWIDGETSIZE_MAX);
  layout->addWidget(field, /*stretch=*/1);
  for (QWidget* button : buttons) layout->addWidget(button);
  return group;
}

void SettingRow::SetModified(bool modified) {
  modified_ = modified;
  ShowRevert();
  if (included_in_ != nullptr) included_in_->ShowRevert();
}

void SettingRow::SetOverridden(bool overridden) {
  overridden_ = overridden;
  ShowRevert();
  if (included_in_ != nullptr) included_in_->ShowRevert();
}

void SettingRow::Include(SettingRow* other) {
  other->included_in_ = this;
  included_.push_back(other);
  ShowRevert();
}

void SettingRow::ShowRevert() {
  const auto any = [this](bool SettingRow::* flag) {
    return this->*flag ||
           std::ranges::any_of(included_, [flag](const SettingRow* row) { return row->*flag; });
  };
  const bool modified = any(&SettingRow::modified_);
  dot_->setVisible(modified);
  revert_->setVisible(modified || any(&SettingRow::overridden_));
  revert_->setToolTip(modified ? "Undo this change" : "Use Settings' value");
  revert_->setAccessibleName((modified ? "Undo the change to " : "Use Settings' value for ") +
                             label_->text());
}

}  // namespace mira_gui
