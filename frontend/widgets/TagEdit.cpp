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
#include "FlowLayout.h"

namespace mira_gui {

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
