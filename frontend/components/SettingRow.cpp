#include "SettingRow.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionFrame>
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

  label_ = new WrapWhenNarrowLabel(label, this);  // a long label in a narrow card takes two lines
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
  icons::Follow(revert_, icons::Glyph::Reset, &theme::Tokens::text_muted);
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

namespace {
constexpr const char* kControlLine = "control_below";
// A FieldWithButtons' width beside its label, and how much of it its buttons take.
constexpr const char* kBesideWidth = "beside_width";
constexpr const char* kBesideButtons = "beside_buttons";
// The narrowest a field beside its label gets before it goes under the label.
constexpr int kLeastBesideField = 160;

// The control's text field, if it has one (a line edit, or an editable combo box's).
QLineEdit* TextField(QWidget* control) {
  if (auto* edit = qobject_cast<QLineEdit*>(control)) return edit;
  return control->findChild<QLineEdit*>();
}

// How wide `edit` must be to show all of its text, or its placeholder when empty.
int WidthForText(const QLineEdit* edit) {
  const QString text = edit->text().isEmpty() ? edit->placeholderText() : edit->text();
  if (text.isEmpty()) return 0;
  QStyleOptionFrame option;
  option.initFrom(edit);
  option.lineWidth = edit->hasFrame()
                         ? edit->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, edit)
                         : 0;
  const QMargins margins = edit->textMargins();
  const QFontMetrics metrics = edit->fontMetrics();
  // 4 for QLineEdit's own side margins, as its sizeHint counts them.
  const QSize content(metrics.horizontalAdvance(text) + margins.left() + margins.right() + 4,
                      metrics.height());
  return edit->style()->sizeFromContents(QStyle::CT_LineEdit, &option, content, edit).width();
}
}  // namespace

int SettingRow::LabelLeft() const {
  return dot_->width() + 4 + line_->spacing();  // the dot and its gaps
}

QHBoxLayout* SettingRow::AddLineBelow(QWidget* widget, int left, int gap, int stretch) {
  auto* below = new QHBoxLayout();
  const QMargins line = line_->contentsMargins();
  below->setContentsMargins(line.left() + left, 0, line.right(), line.bottom());
  line_->setContentsMargins(line.left(), line.top(), line.right(), gap);
  below->addWidget(widget, stretch);
  outer_->addLayout(below);
  return below;
}

void SettingRow::AddControlBelow(QWidget* control) {
  line_bottom_ = line_->contentsMargins().bottom();
  movable_ = control;
  movable_min_ = control->minimumWidth();
  below_line_ = AddLineBelow(control, LabelLeft(), 4, 1);
  below_line_->setObjectName(kControlLine);
  // A field beside the label shows its whole text, so new text can move it under the label.
  if (QLineEdit* field = TextField(control))
    connect(field, &QLineEdit::textChanged, this, [this] { PlaceControl(); });
  PlaceControl();
}

void SettingRow::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  PlaceControl();
}

void SettingRow::PlaceControl() {
  if (movable_ == nullptr || width() == 0) return;
  const int hint = std::max(movable_min_, movable_->sizeHint().width());
  int full = std::max(hint, movable_->property(kBesideWidth).toInt());
  int least = hint;
  if (const QVariant buttons = movable_->property(kBesideButtons); buttons.isValid()) {
    const QLineEdit* field = TextField(movable_);
    const int text = field != nullptr ? WidthForText(field) : 0;
    least = std::max(least, buttons.toInt() + std::max(kLeastBesideField, text));
    full = std::max(full, least);
  }
  const QMargins margins = line_->contentsMargins();
  const int spacing = line_->spacing();
  // The line's widgets up to the label's end, each with the spacing after it (spacers add only
  // their own width), then the revert button's kept space.
  int used = margins.left() + margins.right();
  const int label_at = line_->indexOf(label_);
  for (int i = 0; i < label_end_; ++i) {
    QLayoutItem* item = line_->itemAt(i);
    QWidget* widget = item->widget();
    if (widget == nullptr) {
      used += item->sizeHint().width();
      continue;
    }
    if (widget->isHidden() && !widget->sizePolicy().retainSizeWhenHidden()) continue;
    used += (i == label_at ? label_->sizeHint().width() : item->sizeHint().width()) + spacing;
  }
  if (revert_->sizePolicy().retainSizeWhenHidden() || !revert_->isHidden())
    used += revert_->sizeHint().width() + spacing;
  const int room = width() - used;
  const bool beside = room >= least;
  if (beside) movable_->setFixedWidth(std::min(room, full));
  if (beside == beside_) return;
  beside_ = beside;
  const QMargins line = line_->contentsMargins();
  if (beside) {
    below_line_->removeWidget(movable_);
    line_->addWidget(movable_, 0, Qt::AlignVCenter);
    line_->setContentsMargins(line.left(), line.top(), line.right(), line_bottom_);
    below_margins_ = below_line_->contentsMargins();
    below_line_->setContentsMargins(0, 0, 0, 0);  // empty, it adds no height
  } else {
    line_->removeWidget(movable_);
    movable_->setMinimumWidth(movable_min_);
    movable_->setMaximumWidth(QWIDGETSIZE_MAX);
    below_line_->setContentsMargins(below_margins_);
    below_line_->addWidget(movable_, 1);
    line_->setContentsMargins(line.left(), line.top(), line.right(), 4);
  }
}

void SettingRow::SetBelow(QWidget* widget) {
  AddLineBelow(widget, 16, 6, 0);
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
      const int left = below->objectName() == kControlLine ? LabelLeft() : 16;
      below->setContentsMargins(8 + pixels + left, m.top(), m.right(), m.bottom());
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
  auto* layout = new QHBoxLayout(group);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  field->setMinimumWidth(0);
  field->setMaximumWidth(QWIDGETSIZE_MAX);
  layout->addWidget(field, /*stretch=*/1);
  // Beside its label, the field is as wide as a field without buttons.
  int width = kFieldColumnWidth;
  for (QWidget* button : buttons) {
    layout->addWidget(button);
    width += layout->spacing() + button->sizeHint().width();
  }
  group->setProperty(kBesideWidth, width);
  group->setProperty(kBesideButtons, width - kFieldColumnWidth);
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

void SettingRow::DropUndo() {
  KeepSpaceWhenHidden(revert_, false);
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
