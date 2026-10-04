#include "SettingsCard.h"

#include <algorithm>

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
#include <QTimer>
#include <QToolButton>
#include <QTransform>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "Icons.h"
#include "SettingsColumns.h"
#include "SettingsNav.h"
#include "Theme.h"
#include "ToolTip.h"

namespace mira_gui {
namespace {

constexpr int kRowPaddingX = 18;

// Hidden widgets keep their space, so revealing one on hover doesn't shift the row.
void KeepSpaceWhenHidden(QWidget* widget) {
  QSizePolicy policy = widget->sizePolicy();
  policy.setRetainSizeWhenHidden(true);
  widget->setSizePolicy(policy);
}

}  // namespace

// --- ElidedLabel ----------------------------------------------------------

ElidedLabel::ElidedLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {}

QSize ElidedLabel::minimumSizeHint() const { return QSize(20, QLabel::minimumSizeHint().height()); }

void ElidedLabel::paintEvent(QPaintEvent*) {
  const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, width());
  // Here rather than on resize, since setText isn't virtual.
  setToolTip(shown == text() ? QString() : text());
  QPainter painter(this);
  painter.setPen(palette().color(QPalette::WindowText));
  painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, shown);
}

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

QSize Switch::sizeHint() const { return {36, 22}; }

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
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
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

  if (hasFocus()) {
    painter.setPen(QPen(tokens.accent, 1, Qt::DotLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(track.adjusted(-2, -2, 2, 2), 12, 12);
  }
}

// --- SettingRow -----------------------------------------------------------

SettingRow::SettingRow(const QString& label, const QString& doc, QWidget* parent)
    : QWidget(parent) {
  setObjectName("setting_row");
  setAttribute(Qt::WA_StyledBackground);  // so a row being dragged can be highlighted
  outer_ = new QVBoxLayout(this);
  outer_->setContentsMargins(8, 8, kRowPaddingX, 8);
  outer_->setSpacing(6);

  line_ = new QHBoxLayout();
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
  label_->setMinimumHeight(30);
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
  revert_->setIcon(icons::For(icons::Glyph::Undo, theme::Current().text_muted));
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
  grip->setIcon(icons::For(icons::Glyph::Grip, theme::Current().text_muted));
  grip->setCursor(Qt::OpenHandCursor);
  grip->setToolTip("Drag to reorder, or press Alt+Up or Alt+Down");
  grip->setAccessibleName("Reorder " + label_->text());
  line_->insertWidget(line_->indexOf(label_), grip, 0, Qt::AlignVCenter);
  ++label_end_;
  grip_ = grip;
}

void SettingRow::AddControl(QWidget* control, int stretch) { line_->addWidget(control, stretch, Qt::AlignVCenter); }

void SettingRow::SetLeading(QWidget* widget) {
  line_->insertWidget(line_->indexOf(label_), widget, 0, Qt::AlignVCenter);
  ++label_end_;
}

void SettingRow::AddAfterLabel(QWidget* widget) { line_->insertWidget(label_end_++, widget, 0, Qt::AlignVCenter); }

void SettingRow::SetBelow(QWidget* widget) {
  auto* indent = new QHBoxLayout();
  indent->setContentsMargins(16, 0, 0, 0);
  indent->addWidget(widget);
  outer_->addLayout(indent);
}

void SettingRow::SetModified(bool modified) {
  dot_->setVisible(modified);
  revert_->setVisible(modified);
}

// --- SettingsCard ---------------------------------------------------------

SettingsCard::SettingsCard(const QString& title, QWidget* parent) : QFrame(parent) {
  setObjectName("settings_card");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 4);
  layout->setSpacing(0);

  header_ = new QWidget(this);
  header_layout_ = new QHBoxLayout(header_);
  header_layout_->setContentsMargins(kRowPaddingX, 14, kRowPaddingX - 6, 4);
  header_layout_->setSpacing(10);
  title_ = new QLabel(title, header_);
  title_->setProperty("role", "section");
  header_layout_->addWidget(title_, /*stretch=*/1);
  header_->setVisible(!title.isEmpty());
  layout->addWidget(header_);

  body_ = new QWidget(this);
  body_layout_ = new QVBoxLayout(body_);
  body_layout_->setContentsMargins(0, 0, 0, 0);
  body_layout_->setSpacing(0);
  layout->addWidget(body_);
  layout->addStretch(1);  // a card stretched to its column's bottom keeps its rows at the top
}

void SettingsCard::AddRow(QWidget* row) {
  body_layout_->addWidget(row);
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
  update();
}

QString SettingsCard::Title() const { return title_->text(); }

void SettingsCard::MoveRow(QWidget* row, int to) {
  const int from = static_cast<int>(rows_.indexOf(row));
  to = std::clamp(to, 0, static_cast<int>(rows_.size()) - 1);
  if (from < 0 || from == to) return;
  rows_.move(from, to);
  body_layout_->removeWidget(row);
  body_layout_->insertWidget(to, row);
  dragged_ = true;
  update();
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
      const int y = body_->mapFromGlobal(static_cast<QMouseEvent*>(event)->globalPosition().toPoint()).y();
      for (int i = 0; i < rows_.size(); ++i) {
        const QWidget* other = rows_[i];
        if (other->isVisible() && other != row && y >= other->y() && y < other->y() + other->height()) {
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
      if (key->modifiers() != Qt::AltModifier || (key->key() != Qt::Key_Up && key->key() != Qt::Key_Down)) {
        return false;
      }
      dragged_ = false;
      MoveRow(row, static_cast<int>(rows_.indexOf(row)) + (key->key() == Qt::Key_Up ? -1 : 1));
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
    chevron_ = new QToolButton(header_);
    chevron_->setAutoRaise(true);
    chevron_->setFocusPolicy(Qt::StrongFocus);
    connect(chevron_, &QToolButton::clicked, this, [this] { SetExpanded(!Expanded()); });
    header_layout_->addWidget(chevron_);
    header_->setCursor(Qt::PointingHandCursor);
    header_->installEventFilter(this);
    header_->show();
  }
  SetExpanded(!collapsed);
}

void SettingsCard::SetExpanded(bool expanded) {
  body_->setVisible(expanded);
  if (chevron_ != nullptr) {
    const QColor muted = theme::Current().text_muted;
    QIcon icon = icons::For(icons::Glyph::ChevronDown, muted);
    if (expanded) icon = QIcon(icon.pixmap(32).transformed(QTransform().rotate(180)));
    chevron_->setIcon(icon);
    chevron_->setToolTip(expanded ? "Fold" : "Show");
    // Folded, the header is the whole card, so it gets the bottom padding.
    header_layout_->setContentsMargins(kRowPaddingX, 12, kRowPaddingX - 6, expanded ? 4 : 12);
  }
  update();
}

bool SettingsCard::Expanded() const { return body_->isVisibleTo(this); }

bool SettingsCard::eventFilter(QObject* watched, QEvent* event) {
  if (auto* grip = qobject_cast<QWidget*>(watched); grip != nullptr && grip->objectName() == "row_grip") {
    if (HandleGrip(grip, event)) return true;
  }
  if (watched == header_ && event->type() == QEvent::MouseButtonRelease) {
    // A click on a header control (a switch, a button) is that control's, not a fold.
    auto* mouse = static_cast<QMouseEvent*>(event);
    for (QWidget* hit = header_->childAt(mouse->position().toPoint()); hit != nullptr && hit != header_;
         hit = hit->parentWidget()) {
      if (qobject_cast<QAbstractButton*>(hit) != nullptr) return QFrame::eventFilter(watched, event);
    }
    SetExpanded(!Expanded());
    return true;
  }
  return QFrame::eventFilter(watched, event);
}

void SettingsCard::paintEvent(QPaintEvent* event) {
  QFrame::paintEvent(event);
  if (!body_->isVisible()) return;
  QPainter painter(this);
  QColor line = theme::Current().border;
  line.setAlpha(150);
  painter.setPen(line);
  bool first = true;
  for (QWidget* row : rows_) {
    if (!row->isVisible()) continue;
    if (first) {
      first = false;
      continue;
    }
    const int y = body_->y() + row->y();
    painter.drawLine(QPoint(1, y), QPoint(width() - 2, y));
  }
}

// --- SettingsPage ---------------------------------------------------------

namespace {

// A page's cards in adaptive columns (ui/SettingsColumns). The split is kept
// until the width or the set of shown cards changes, so folding a card never
// moves cards between columns. The last card of a shorter column grows down to
// the tallest column's bottom, unless it can fold.
class CardColumns : public QLayout {
public:
  explicit CardColumns(QWidget* parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }
  ~CardColumns() override {
    while (QLayoutItem* item = takeAt(0)) delete item;
  }

  void addItem(QLayoutItem* item) override {
    items_.append(item);
    invalidate();
  }
  int count() const override { return static_cast<int>(items_.size()); }
  QLayoutItem* itemAt(int index) const override { return items_.value(index); }
  QLayoutItem* takeAt(int index) override {
    if (index < 0 || index >= items_.size()) return nullptr;
    plans_.clear();
    return items_.takeAt(index);
  }
  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return Place(QRect(0, 0, width, 0), /*apply=*/false); }
  QSize sizeHint() const override { return {settings_columns::kMaxWidth, heightForWidth(settings_columns::kMaxWidth)}; }
  QSize minimumSize() const override {
    int width = 0;
    for (const QLayoutItem* item : items_) {
      if (!item->isEmpty()) width = std::max(width, item->minimumSize().width());
    }
    return {width, 0};
  }
  void setGeometry(const QRect& rect) override {
    QLayout::setGeometry(rect);
    Place(rect, /*apply=*/true);
  }
  void Replan() {
    plans_.clear();
    invalidate();
  }

private:
  static int HeightOf(const QLayoutItem* item, int width) {
    return item->hasHeightForWidth() ? item->heightForWidth(width) : item->sizeHint().height();
  }

  static bool CanGrow(const QLayoutItem* item) {
    const auto* card = qobject_cast<const SettingsCard*>(item->widget());
    return card != nullptr && !card->Collapsible();
  }

  // Positions the shown cards in `rect` (when `apply`); returns the tallest column's height.
  int Place(const QRect& rect, bool apply) const {
    std::vector<int> shown;
    for (int i = 0; i < items_.size(); ++i) {
      if (!items_[i]->isEmpty()) shown.push_back(i);
    }
    // Plans are kept per width, so a size probe at another width can't replace
    // the one in use; a real resize or another set of cards starts over.
    if (shown != shown_ || (apply && rect.width() != applied_width_)) plans_.clear();
    shown_ = shown;
    if (apply) applied_width_ = rect.width();
    auto plan_it = plans_.find(rect.width());
    if (plan_it == plans_.end()) {
      const int measure = std::min(settings_columns::kMaxWidth, rect.width());
      std::vector<int> heights;
      for (const int i : shown) heights.push_back(HeightOf(items_[i], measure));
      plan_it = plans_.insert(rect.width(), settings_columns::Arrange(heights, rect.width()));
    }
    const settings_columns::Plan& plan = plan_it.value();

    std::vector<int> natural;
    for (const auto& column : plan.columns) {
      int height = 0;
      for (const int c : column) height += HeightOf(items_[shown_[c]], plan.width) + kCardGap;
      natural.push_back(column.empty() ? 0 : height - kCardGap);
    }
    const int tallest = natural.empty() ? 0 : *std::ranges::max_element(natural);
    if (!apply) return tallest;

    for (size_t j = 0; j < plan.columns.size(); ++j) {
      const auto& column = plan.columns[j];
      const int x = rect.x() + static_cast<int>(j) * (plan.width + settings_columns::kGap);
      int y = rect.y();
      for (const int c : column) {
        QLayoutItem* item = items_[shown_[c]];
        int height = HeightOf(item, plan.width);
        if (c == column.back() && CanGrow(item)) height += std::max(0, rect.height() - natural[j]);
        item->setGeometry(QRect(x, y, plan.width, height));
        y += height + kCardGap;
      }
    }
    return tallest;
  }

  static constexpr int kCardGap = 16;

  QList<QLayoutItem*> items_;
  mutable std::vector<int> shown_;  // indices into items_ the plans were made for
  mutable int applied_width_ = -1;
  mutable QHash<int, settings_columns::Plan> plans_;  // by width
};

}  // namespace

SettingsPage::SettingsPage(const QString& title, QWidget* parent) : QWidget(parent) {
  setObjectName("settings_section");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);
  title_ = new QLabel(title, this);
  title_->setProperty("role", "heading");
  layout->addWidget(title_);
  cards_ = new QWidget(this);
  new CardColumns(cards_);
  layout->addWidget(cards_);
}

SettingsCard* SettingsPage::AddCard(const QString& title) {
  auto* card = new SettingsCard(title);
  AddWidget(card);
  return card;
}

void SettingsPage::AddWidget(QWidget* widget) { cards_->layout()->addWidget(widget); }

QString SettingsPage::Title() const { return title_->text(); }

void SettingsPage::Rearrange() { static_cast<CardColumns*>(cards_->layout())->Replan(); }

void SettingsPage::SetGroupHeading(const QString& text) {
  auto* heading = new QLabel(text.toUpper(), this);
  heading->setProperty("role", "nav_heading");
  static_cast<QVBoxLayout*>(layout())->insertWidget(0, heading);
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

int ChangeBar::RoomNeeded() const { return height() + 18; }

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
