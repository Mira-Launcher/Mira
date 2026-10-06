#include "TagEdit.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>

#include <algorithm>

#include "../theme/Icons.h"

namespace mira_gui {
namespace {

// Lays its items out left to right, wrapping onto a new line when one won't fit.
class FlowLayout : public QLayout {
public:
  explicit FlowLayout(QWidget* parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }
  ~FlowLayout() override {
    while (QLayoutItem* item = takeAt(0)) delete item;
  }

  void addItem(QLayoutItem* item) override { items_.append(item); }
  int count() const override { return static_cast<int>(items_.size()); }
  QLayoutItem* itemAt(int index) const override { return items_.value(index); }
  QLayoutItem* takeAt(int index) override {
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
  }
  // Moves the item for `widget` to `index`, e.g. to keep a trailing button last.
  void Move(QWidget* widget, int index) {
    const int from = indexOf(widget);
    if (from >= 0) items_.move(from, std::clamp(index, 0, count() - 1));
    invalidate();
  }

  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return Arrange(QRect(0, 0, width, 0), false); }
  void setGeometry(const QRect& rect) override {
    QLayout::setGeometry(rect);
    Arrange(rect, true);
  }
  QSize sizeHint() const override { return minimumSize(); }
  QSize minimumSize() const override {
    QSize size;
    for (const QLayoutItem* item : items_) size = size.expandedTo(item->minimumSize());
    return size;
  }

private:
  static constexpr int kGap = 6;

  int Arrange(const QRect& rect, bool apply) const {
    int x = rect.x();
    int y = rect.y();
    int line_height = 0;
    for (QLayoutItem* item : items_) {
      if (item->widget() != nullptr && item->widget()->isHidden()) continue;
      const QSize hint = item->sizeHint();
      if (x > rect.x() && x + hint.width() > rect.right() + 1) {
        x = rect.x();
        y += line_height + kGap;
        line_height = 0;
      }
      line_height = std::max(line_height, hint.height());
      if (apply) item->setGeometry(QRect(QPoint(x, y), hint));
      x += hint.width() + kGap;
    }
    return y + line_height - rect.y();
  }

  QList<QLayoutItem*> items_;
};

}  // namespace

TagEdit::TagEdit(QWidget* parent) : QWidget(parent) {
  auto* layout = new FlowLayout(this);

  add_ = new QPushButton("Add tag", this);
  add_->setObjectName("text_button");
  icons::Follow(add_, icons::Glyph::Plus);
  connect(add_, &QPushButton::clicked, this, &TagEdit::StartAdding);
  layout->addWidget(add_);

  input_ = new QLineEdit(this);
  input_->setObjectName("tag_input");
  input_->setPlaceholderText("New tag");
  input_->setFixedWidth(150);
  input_->hide();
  input_->installEventFilter(this);
  connect(input_, &QLineEdit::returnPressed, this, &TagEdit::FinishAdding);
  // Leaving it keeps whatever was typed, as Enter would: Tab here, a click in eventFilter.
  connect(input_, &QLineEdit::editingFinished, this, [this] {
    if (input_->isVisible() && !input_->hasFocus()) FinishAdding();
  });
  layout->addWidget(input_);
}

void TagEdit::SetTags(const std::vector<std::string>& tags) {
  tags_ = tags;
  Rebuild();
}

void TagEdit::SetSuggestions(const QStringList& tags) {
  auto* completer = new QCompleter(tags, input_);
  completer->setCaseSensitivity(Qt::CaseInsensitive);
  delete input_->completer();
  input_->setCompleter(completer);
}

void TagEdit::Rebuild() {
  // Later, not now: a click on a chip's own button can be what triggered this.
  for (QWidget* chip : chips_) {
    chip->hide();
    chip->deleteLater();
  }
  chips_.clear();
  auto* flow = static_cast<FlowLayout*>(layout());
  for (const std::string& tag : tags_) {
    auto* chip = new QFrame(this);
    chip->setObjectName("tag_chip");
    auto* chip_layout = new QHBoxLayout(chip);
    chip_layout->setContentsMargins(10, 2, 4, 2);
    chip_layout->setSpacing(4);
    chip_layout->addWidget(new QLabel(QString::fromStdString(tag), chip));
    auto* remove = new QToolButton(chip);
    remove->setObjectName("tag_remove");
    icons::Follow(remove, icons::Glyph::Close);
    remove->setIconSize(QSize(12, 12));
    remove->setToolTip(QString("Remove %1").arg(QString::fromStdString(tag)));
    connect(remove, &QToolButton::clicked, this, [this, tag] {
      std::erase(tags_, tag);
      Rebuild();
      emit Changed();
    });
    chip_layout->addWidget(remove);
    flow->addWidget(chip);
    flow->Move(chip, static_cast<int>(chips_.size()));
    chips_.append(chip);
  }
  updateGeometry();
}

void TagEdit::StartAdding() {
  add_->hide();
  input_->clear();
  input_->show();
  input_->setFocus();
  // A click on something that takes no focus (the card itself) never ends the edit on its own.
  qApp->installEventFilter(this);
}

bool TagEdit::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::MouseButtonPress && input_->isVisible()) {
    auto* target = qobject_cast<QWidget*>(watched);
    const QWidget* popup = input_->completer() != nullptr ? input_->completer()->popup() : nullptr;
    const bool inside = target != nullptr && (target == input_ || input_->isAncestorOf(target) ||
                                              (popup != nullptr && (target == popup || popup->isAncestorOf(target))));
    if (target != nullptr && !inside) FinishAdding();
    return false;
  }
  // Esc drops the new tag instead of reaching the window's Esc shortcut, which would close the card.
  if (watched != input_ || (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress) ||
      static_cast<QKeyEvent*>(event)->key() != Qt::Key_Escape) {
    return QWidget::eventFilter(watched, event);
  }
  event->accept();
  if (event->type() == QEvent::KeyPress) {
    input_->clear();
    FinishAdding();
  }
  return true;
}

void TagEdit::FinishAdding() {
  if (!input_->isVisible()) return;
  qApp->removeEventFilter(this);
  // Takes several at once, comma-separated.
  bool added = false;
  for (const QString& part : input_->text().split(',', Qt::SkipEmptyParts)) {
    const std::string tag = part.trimmed().toStdString();
    if (tag.empty() || std::ranges::find(tags_, tag) != tags_.end()) continue;
    tags_.push_back(tag);
    added = true;
  }
  input_->hide();
  input_->clear();
  add_->show();
  if (!added) return;
  Rebuild();
  emit Changed();
}

}  // namespace mira_gui
