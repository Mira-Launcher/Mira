#include "SettingsCard.h"

#include <QApplication>
#include <QCursor>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QTransform>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>

#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/Scrolling.h"
#include "../widgets/ToolTip.h"
#include "SettingsNav.h"

namespace mira_gui {
namespace {

constexpr int kRowPaddingX = 18;
// The settings column's width, and on windows at least kWideRoom wide.
constexpr int kColumn = 720;
constexpr int kWideColumn = 760;
constexpr int kWideRoom = 1400;

// The chevron that folds a card or an expander row, redrawn in a new theme's color.
QToolButton* NewChevron(QWidget* parent, QWidget* owner, std::function<bool()> expanded,
                        std::function<void(bool)> set_expanded) {
  auto* chevron = new QToolButton(parent);
  chevron->setAutoRaise(true);
  QObject::connect(chevron, &QToolButton::clicked, owner,
                   [expanded, set_expanded] { set_expanded(!expanded()); });
  QObject::connect(theme::Notifier::Instance(), &theme::Notifier::Changed, chevron,
                   [expanded, set_expanded] { set_expanded(expanded()); });
  return chevron;
}

void ShowChevron(QToolButton* chevron, bool expanded) {
  chevron->setIcon(icons::For(expanded ? icons::Glyph::ChevronUp : icons::Glyph::ChevronDown,
                              theme::Current().text_muted));
  chevron->setToolTip(expanded ? "Fold" : "Show");
}

// A click released on `head` that folds it: one on a control in it (a switch, a button) is that
// control's.
bool FoldClick(QWidget* head, QEvent* event) {
  if (event->type() != QEvent::MouseButtonRelease) return false;
  const auto* mouse = static_cast<QMouseEvent*>(event);
  for (QWidget* hit = head->childAt(mouse->position().toPoint()); hit != nullptr && hit != head;
       hit = hit->parentWidget()) {
    if (qobject_cast<QAbstractButton*>(hit) != nullptr) return false;
  }
  return true;
}

// A card's rows, with a line between each two shown. Drawn here rather than by the card so the
// lines scroll with the rows.
class CardBody : public QWidget {
 public:
  CardBody(const QList<QWidget*>* rows, QWidget* parent) : QWidget(parent), rows_(rows) {}

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    QColor line = theme::Current().border;
    line.setAlpha(150);
    painter.setPen(line);
    bool first = true;
    for (QWidget* row : *rows_) {
      if (!row->isVisible()) continue;
      if (first) {
        first = false;
        continue;
      }
      painter.drawLine(QPoint(1, row->y()), QPoint(width() - 2, row->y()));
    }
  }

 private:
  const QList<QWidget*>* rows_;
};

// The rows under a scrolling card's rows, with a line over each shown, the first included.
class CardFooter : public QWidget {
 public:
  using QWidget::QWidget;

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    QColor line = theme::Current().border;
    line.setAlpha(150);
    painter.setPen(line);
    for (QObject* child : children()) {
      auto* row = qobject_cast<QWidget*>(child);
      if (row == nullptr || !row->isVisible()) continue;
      painter.drawLine(QPoint(1, row->y()), QPoint(width() - 2, row->y()));
    }
  }
};

bool HasGrip(const QWidget* row) {
  const auto* setting = qobject_cast<const SettingRow*>(row);
  return setting != nullptr && setting->Grip() != nullptr && !setting->Grip()->isHidden();
}

}  // namespace

// --- Switch ---------------------------------------------------------------

Switch::Switch(QWidget* parent) : QAbstractButton(parent) {
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  slide_ = new QVariantAnimation(this);
  slide_->setDuration(120);
  connect(slide_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    position_ = value.toReal();
    update();
  });
  connect(this, &QAbstractButton::toggled, this, [this](bool on) {
    slide_->stop();
    if (!isVisible()) {
      position_ = on ? 1 : 0;
      update();
      return;
    }
    slide_->setStartValue(position_);
    slide_->setEndValue(on ? 1.0 : 0.0);
    slide_->start();
  });
}

QSize Switch::sizeHint() const {
  return {36, 22};
}

void Switch::paintEvent(QPaintEvent*) {
  // Set with its signals blocked, the switch never saw the toggle that starts the slide.
  if (slide_->state() != QAbstractAnimation::Running) position_ = isChecked() ? 1 : 0;
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF track = QRectF(0, 0, 34, 20).translated(1, (height() - 20) / 2.0);
  const QColor off = tokens.surface_alt;
  const QColor on = isEnabled() ? tokens.accent : tokens.border;
  // Blend the track color with the knob's travel, so the animation carries the color too.
  const auto mix = [](const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
  };
  painter.setPen(QPen(position_ > 0.5 ? on : tokens.border, 1));
  painter.setBrush(mix(off, on, position_));
  painter.drawRoundedRect(track.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);

  const qreal travel = track.width() - 18;
  const QRectF knob(track.left() + 3 + travel * position_, track.top() + 3, 14, 14);
  painter.setPen(Qt::NoPen);
  painter.setBrush(position_ > 0.5 ? tokens.on_accent : tokens.text_muted);
  painter.drawEllipse(knob);
}

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
  theme::SetStyleProperty(meta_, "role", "subtle");
  meta_->hide();
  head_layout_->addWidget(meta_);
  head_layout_->addStretch(1);
  summary_ = new QLabel(head_);
  summary_->setObjectName("expander_summary");
  theme::SetStyleProperty(summary_, "role", "subtle");
  summary_->hide();
  head_layout_->addWidget(summary_);
  chevron_ =
      NewChevron(head_, this, [this] { return Expanded(); }, [this](bool on) { SetExpanded(on); });
  chevron_->setAccessibleName(label);
  head_layout_->addWidget(chevron_);
  head_->installEventFilter(this);
  layout->addWidget(head_);

  body_ = new CardBody(&rows_, this);
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

void ExpanderRow::SetMeta(const QString& text) {
  meta_->setText(text);
  meta_->setVisible(!text.isEmpty());
}

void ExpanderRow::SetSummary(const QString& text, const QString& tone) {
  summary_->setText(text);
  summary_->setVisible(!text.isEmpty());
  theme::SetStyleProperty(summary_, "tone", tone);
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
  ShowChevron(chevron_, expanded);
  if (changed) emit Toggled(expanded);
}

bool ExpanderRow::Expanded() const {
  return body_->isVisibleTo(this);
}

bool ExpanderRow::eventFilter(QObject* watched, QEvent* event) {
  if (watched == head_ && FoldClick(head_, event)) {
    SetExpanded(!Expanded());
    return true;
  }
  return QWidget::eventFilter(watched, event);
}

int SettingsColumnWidth(int room) {
  return std::min(room, room >= kWideRoom ? kWideColumn : kColumn);
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

// --- SettingsCard ---------------------------------------------------------

SettingsCard::SettingsCard(const QString& title, QWidget* parent) : QFrame(parent) {
  setObjectName("settings_card");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(1, 1, 1, 1);  // inside its border, so a sunken row never covers it
  layout->setSpacing(0);

  header_ = new QWidget(this);
  header_layout_ = new QHBoxLayout(header_);
  // Its title starts where the rows' labels do (SettingRow's line, past its dot's room).
  header_layout_->setContentsMargins(kLabelInset, 14, kRowPaddingX - 6, 4);
  header_layout_->setSpacing(10);
  title_ = new QLabel(title, header_);
  title_->setProperty("role", "section");
  header_layout_->addWidget(title_, /*stretch=*/1);
  header_->setVisible(!title.isEmpty());
  layout->addWidget(header_);

  body_ = new CardBody(&rows_, this);
  body_layout_ = new QVBoxLayout(body_);
  body_layout_->setContentsMargins(0, 0, 0, 0);
  body_layout_->setSpacing(0);
  layout->addWidget(body_);
  layout->addStretch(1);  // a card stretched to its column's bottom keeps its rows at the top
  footer_ = new CardFooter(this);
  footer_layout_ = new QVBoxLayout(footer_);
  footer_layout_->setContentsMargins(0, 0, 0, 0);
  footer_layout_->setSpacing(0);
  footer_->hide();
  layout->addWidget(footer_);
}

void SettingsCard::AddFooterRow(QWidget* row) {
  row->setParent(footer_);
  footer_layout_->addWidget(row);
  footer_->show();
}

void SettingsCard::AddRow(QWidget* row) {
  // Before the stretch a scrolling card keeps under its rows.
  body_layout_->insertWidget(static_cast<int>(rows_.size()), row);
  rows_.append(row);
  if (auto* setting = qobject_cast<SettingRow*>(row)) {
    tooltip::AlignLeftWith(setting->Label(), this);
    if (setting->Grip() != nullptr) setting->Grip()->installEventFilter(this);
  }
}

void SettingsCard::ClearRows() {
  for (QWidget* row : rows_) {
    body_layout_->removeWidget(row);
    row->hide();
    row->deleteLater();  // one of its buttons may be what asked for the rebuild
  }
  rows_.clear();
  dragging_ = nullptr;
  body_->update();
}

void SettingsCard::RemoveRow(QWidget* row) {
  if (!rows_.removeOne(row)) return;
  body_layout_->removeWidget(row);
  row->hide();
  row->deleteLater();
  if (dragging_ == row) dragging_ = nullptr;
  body_->update();
}

void SettingsCard::SetScrollable() {
  if (scroll_ != nullptr) return;
  auto* layout = static_cast<QVBoxLayout*>(this->layout());
  scroll_ = new QScrollArea(this);
  scroll_->setObjectName("card_scroll");
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
  // A pinned row's columns stay over the rows' while the scroll bar takes room beside them.
  connect(scroll_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int max) {
    if (pinned_ != nullptr)
      pinned_->setContentsMargins(0, 0, max > 0 ? scroll_->verticalScrollBar()->width() : 0, 0);
  });
}

void SettingsCard::SetPinnedRow(QWidget* row) {
  auto* layout = static_cast<QVBoxLayout*>(this->layout());
  pinned_ = new QWidget(this);
  auto* pinned_layout = new QVBoxLayout(pinned_);
  pinned_layout->setContentsMargins(0, 0, 0, 0);
  pinned_layout->addWidget(row);
  layout->insertWidget(layout->indexOf(header_) + 1, pinned_);
}

QString SettingsCard::Title() const {
  return title_->text();
}

void SettingsCard::MoveRow(QWidget* row, int to) {
  const int from = static_cast<int>(rows_.indexOf(row));
  to = std::clamp(to, 0, static_cast<int>(rows_.size()) - 1);
  if (from < 0 || from == to) return;
  rows_.move(from, to);
  body_layout_->removeWidget(row);
  body_layout_->insertWidget(to, row);
  dragged_ = true;
  body_->update();
}

bool SettingsCard::HandleGrip(QWidget* grip, QEvent* event) {
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

void SettingsCard::SetLeading(QWidget* widget) {
  header_layout_->insertWidget(0, widget, 0, Qt::AlignVCenter);
  header_->show();
}

void SettingsCard::SetTitle(const QString& title) {
  title_->setText(title);
  header_->setVisible(!title.isEmpty());
}

void SettingsCard::SetTitleWidget(QWidget* widget) {
  const int index = header_layout_->indexOf(title_);
  header_layout_->insertWidget(index, widget, /*stretch=*/1);
  title_->hide();
  header_layout_->setStretchFactor(title_, 0);
  header_->show();
}

void SettingsCard::SetProminentTitle() {
  theme::SetStyleProperty(title_, "role", "heading");
  header_layout_->setContentsMargins(kRowPaddingX + 2, 16, 10, 6);
}

void SettingsCard::SetResettable(bool resettable) {
  if (reset_ == nullptr) {
    reset_ = new QToolButton(header_);
    reset_->setObjectName("card_reset");
    reset_->setAutoRaise(true);
    reset_->setText("Reset to defaults");
    // No taller than the title, and its space kept while hidden, so the header never moves.
    reset_->setFixedHeight(title_->sizeHint().height());
    KeepSpaceWhenHidden(reset_);
    connect(reset_, &QToolButton::clicked, this, &SettingsCard::ResetClicked);
    // Before the chevron, so a folding card keeps it at the far right.
    const int at =
        chevron_ != nullptr ? header_layout_->indexOf(chevron_) : header_layout_->count();
    header_layout_->insertWidget(at, reset_, 0, Qt::AlignVCenter);
    header_->show();
  }
  reset_->setVisible(resettable);
}

void SettingsCard::SetCollapsible(bool collapsed) {
  if (chevron_ == nullptr) {
    chevron_ = NewChevron(
        header_, this, [this] { return Expanded(); }, [this](bool on) { SetExpanded(on); });
    chevron_->setFocusPolicy(Qt::StrongFocus);
    header_layout_->addWidget(chevron_);
    header_->setCursor(Qt::PointingHandCursor);
    header_->installEventFilter(this);
    header_->show();
  }
  SetExpanded(!collapsed);
}

void SettingsCard::SetSummary(const QString& text) {
  if (summary_ == nullptr) {
    summary_ = new QLabel(header_);
    theme::SetStyleProperty(summary_, "role", "subtle");
    const int at =
        chevron_ != nullptr ? header_layout_->indexOf(chevron_) : header_layout_->count();
    header_layout_->insertWidget(at, summary_, 0, Qt::AlignVCenter);
  }
  summary_->setText(text);
}

void SettingsCard::SetExpanded(bool expanded) {
  body_->setVisible(expanded);
  if (chevron_ != nullptr) {
    ShowChevron(chevron_, expanded);
    // Folded, the header is the whole card, so it gets the bottom padding.
    header_layout_->setContentsMargins(kLabelInset, 12, kRowPaddingX - 6, expanded ? 4 : 12);
  }
  update();
}

bool SettingsCard::Expanded() const {
  return body_->isVisibleTo(this);
}

void SettingsCard::resizeEvent(QResizeEvent* event) {
  QFrame::resizeEvent(event);
  RoundBottomRow();  // a list folding or an expander opening resizes the card
}

bool SettingsCard::event(QEvent* event) {
  const bool handled = QFrame::event(event);
  // Rows added or shown inside, which may not resize the card, lay it out again.
  if (event->type() == QEvent::LayoutRequest) RoundBottomRow();
  return handled;
}

void SettingsCard::RoundBottomRow() {
  static const QStringList kSunken = {"list_row", "list_more", "expander_body", "expander_head"};
  for (QWidget* widget : findChildren<QWidget*>()) {
    if (!kSunken.contains(widget->objectName())) continue;
    const int bottom = widget->mapTo(this, QPoint(0, widget->height())).y();
    const bool last = widget->isVisibleTo(this) && bottom >= height() - 2;  // its border's inside
    theme::SetStyleProperty(widget, "card_bottom", last ? "true" : "false");
  }
}

bool SettingsCard::eventFilter(QObject* watched, QEvent* event) {
  if (auto* grip = qobject_cast<QWidget*>(watched);
      grip != nullptr && grip->objectName() == "row_grip") {
    if (HandleGrip(grip, event)) return true;
  }
  if (watched == header_ && FoldClick(header_, event)) {
    SetExpanded(!Expanded());
    return true;
  }
  return QFrame::eventFilter(watched, event);
}

// --- SettingsPage ---------------------------------------------------------

namespace {

// A page's heading, title and cards stacked in one column centered in the page, and a preview
// beside it while there's room (the space either side of the column, less a margin).
class CenteredColumn : public QLayout {
 public:
  explicit CenteredColumn(QWidget* parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }
  ~CenteredColumn() override {
    while (QLayoutItem* item = takeAt(0)) delete item;
  }

  void addItem(QLayoutItem* item) override {
    items_.append(item);
    invalidate();
  }
  void InsertWidget(int index, QWidget* widget) {
    addChildWidget(widget);
    items_.insert(std::clamp(index, 0, static_cast<int>(items_.size())), new QWidgetItem(widget));
    invalidate();
  }
  int count() const override { return static_cast<int>(items_.size()); }
  QLayoutItem* itemAt(int index) const override { return items_.value(index); }
  QLayoutItem* takeAt(int index) override {
    if (index < 0 || index >= items_.size()) return nullptr;
    return items_.takeAt(index);
  }
  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return Place(QRect(0, 0, width, 0), false); }
  QSize sizeHint() const override { return {kWideColumn, heightForWidth(kWideColumn)}; }
  QSize minimumSize() const override {
    int width = 0;
    for (const QLayoutItem* item : items_)
      if (!item->isEmpty()) width = std::max(width, item->minimumSize().width());
    return {std::min(width, 480), 0};
  }
  void setGeometry(const QRect& rect) override {
    QLayout::setGeometry(rect);
    Place(rect, true);
  }
  int ColumnLeft(int width) const { return (width - SettingsColumnWidth(width)) / 2; }

 private:
  static int HeightOf(const QLayoutItem* item, int width) {
    return item->hasHeightForWidth() ? item->heightForWidth(width) : item->sizeHint().height();
  }

  int Place(const QRect& rect, bool apply) const {
    const int column = SettingsColumnWidth(rect.width());
    const int left = rect.x() + ColumnLeft(rect.width());
    int y = rect.y();
    bool first = true;
    bool after_title = false;
    for (QLayoutItem* item : items_) {
      if (item->isEmpty()) continue;
      if (!first) y += after_title ? kTitleGap : kCardGap;
      const int height = HeightOf(item, column);
      if (apply) item->setGeometry(QRect(left, y, column, height));
      y += height;
      first = false;
      after_title = item->widget() != nullptr && item->widget()->objectName() == "settings_title";
    }
    return y - rect.y();
  }

  static constexpr int kCardGap = 16;

  QList<QLayoutItem*> items_;
};

}  // namespace

SettingsPage::SettingsPage(const QString& title, QWidget* parent) : QWidget(parent) {
  setObjectName("settings_section");
  auto* layout = new CenteredColumn(this);
  title_ = new QLabel(title, this);
  title_->setObjectName("settings_title");
  title_->setProperty("role", "heading");
  layout->addWidget(title_);
}

SettingsCard* SettingsPage::AddCard(const QString& title) {
  auto* card = new SettingsCard(title);
  AddWidget(card);
  return card;
}

void SettingsPage::AddWidget(QWidget* widget) {
  layout()->addWidget(widget);
}

QString SettingsPage::Title() const {
  return title_->text();
}

void SettingsPage::Rearrange() {
  layout()->invalidate();
}

void SettingsPage::SetGroupHeading(const QString& text) {
  auto* heading = new QLabel(text.toUpper(), this);
  heading->setObjectName("settings_title");
  heading->setProperty("role", "nav_heading");
  static_cast<CenteredColumn*>(layout())->InsertWidget(0, heading);
}

int SettingsPage::ColumnLeft() const {
  return static_cast<CenteredColumn*>(layout())->ColumnLeft(width());
}

// --- ChangeBar ------------------------------------------------------------

ChangeBar::ChangeBar(QWidget* over) : QFrame(over) {
  setObjectName("change_bar");
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(16, 8, 8, 8);
  layout->setSpacing(10);
  text_ = new QLabel(this);
  layout->addWidget(text_);
  layout->addSpacing(6);
  discard_ = new QPushButton("Discard", this);
  connect(discard_, &QPushButton::clicked, this, &ChangeBar::DiscardClicked);
  layout->addWidget(discard_);
  save_ = new QPushButton("Save", this);
  save_->setDefault(true);
  connect(save_, &QPushButton::clicked, this, &ChangeBar::SaveClicked);
  layout->addWidget(save_);

  auto* shadow = new QGraphicsDropShadowEffect(this);
  shadow->setBlurRadius(28);
  shadow->setOffset(0, 8);
  shadow->setColor(QColor(0, 0, 0, 90));
  setGraphicsEffect(shadow);

  over->installEventFilter(this);
  hide();
}

void ChangeBar::SetCount(int count) {
  SetText(count <= 0   ? QString()
          : count == 1 ? QString("1 unsaved change")
                       : QString("%1 unsaved changes").arg(count));
}

void ChangeBar::SetText(const QString& text, const QString& save, const QString& discard) {
  text_->setText(text);
  save_->setText(save);
  discard_->setText(discard);
  setVisible(!text.isEmpty());
  if (!text.isEmpty()) {
    adjustSize();
    Place();
    raise();
  }
  // The scrolls it floats over: Settings' own (an ancestor, since the bar sits
  // on its content area) or any inside `over`, like a game's Advanced page.
  QList<SettingsNavWidget*> navs = parentWidget()->findChildren<SettingsNavWidget*>();
  for (QWidget* at = parentWidget(); at != nullptr; at = at->parentWidget()) {
    if (auto* nav = qobject_cast<SettingsNavWidget*>(at)) navs.append(nav);
  }
  for (SettingsNavWidget* nav : navs) nav->SetBottomRoom(text.isEmpty() ? 0 : RoomNeeded());
}

int ChangeBar::RoomNeeded() const {
  return height() + 18;
}

void ChangeBar::SetBusy(bool busy) {
  discard_->setEnabled(!busy);
  save_->setEnabled(!busy);
}

bool ChangeBar::eventFilter(QObject* watched, QEvent* event) {
  if (watched == parentWidget() && event->type() == QEvent::Resize) Place();
  return QFrame::eventFilter(watched, event);
}

void ChangeBar::Place() {
  const QWidget* over = parentWidget();
  move((over->width() - width()) / 2, over->height() - height() - 18);
}

}  // namespace mira_gui
