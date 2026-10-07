#include "TagEdit.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>

#include <algorithm>

#include "../client/Types.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
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

void TagEdit::SetOrder(std::vector<std::string> order) {
  order_ = std::move(order);
  Rebuild();
}

void TagEdit::SetFolderPick(const std::string& pick) {
  pick_ = pick;
  Rebuild();
}

std::string TagEdit::FolderTag() const {
  const int index = FolderTagIndex(tags_, folder_tags_, pick_);
  return index >= 0 ? tags_[index] : std::string();
}

void TagEdit::PickFolder(const std::string& tag) {
  pick_.clear();
  const int usual = FolderTagIndex(tags_, folder_tags_);
  if (usual < 0 || !SameTag(tags_[usual], tag)) pick_ = tag;
}

void TagEdit::Remove(const std::string& tag) {
  std::erase(tags_, tag);
  if (SameTag(pick_, tag)) pick_.clear();
  Rebuild();
  emit Changed();
}

void TagEdit::SetSuggestions(const QStringList& tags) {
  auto* completer = new QCompleter(tags, input_);
  completer->setCaseSensitivity(Qt::CaseInsensitive);
  delete input_->completer();
  input_->setCompleter(completer);
}

void TagEdit::SetFolderTags(std::optional<std::vector<std::string>> folder_tags) {
  folder_tags_ = std::move(folder_tags);
  Rebuild();
}

bool TagEdit::IsFolderTag(const std::string& tag) const {
  return folder_tags_ && std::ranges::any_of(*folder_tags_, [&](const std::string& f) { return SameTag(f, tag); });
}

void TagEdit::Rebuild() {
  // Later, not now: a click on a chip's own button can be what triggered this.
  for (QWidget* chip : chips_) {
    chip->hide();
    chip->deleteLater();
  }
  chips_.clear();
  pressed_ = nullptr;
  auto* flow = static_cast<FlowLayout*>(layout());
  const std::string folder_tag = FolderTag();
  for (const std::string& tag : InTagOrder(tags_, order_)) {
    const bool is_folder = !folder_tag.empty() && tag == folder_tag;
    // Another folder tag: a click makes it the game's folder.
    const bool other_folder = !is_folder && IsFolderTag(tag);
    auto* chip = new QFrame(this);
    chip->setObjectName("tag_chip");
    chip->setProperty("tag", QString::fromStdString(tag));
    theme::SetStyleProperty(chip, "folder", is_folder ? "true" : "false");
    if (other_folder) chip->setCursor(Qt::PointingHandCursor);
    if (is_folder) {
      chip->setToolTip(pick_.empty() ? QString("The game's folder, first in the folder tags' order")
                                     : QString("The game's folder, picked for this game"));
    } else if (other_folder) {
      chip->setToolTip("Click to make this the game's folder");
    }
    chip->installEventFilter(this);
    auto* chip_layout = new QHBoxLayout(chip);
    chip_layout->setContentsMargins(is_folder || other_folder ? 6 : 10, 2, 4, 2);
    chip_layout->setSpacing(4);
    if (is_folder || other_folder) {
      auto* icon = new QLabel(chip);
      icons::Follow(icon, icons::Glyph::Folder, 12, is_folder ? &theme::Tokens::accent : &theme::Tokens::text_muted);
      chip_layout->addWidget(icon);
    }
    chip_layout->addWidget(new QLabel(QString::fromStdString(tag), chip));
    auto* remove = new QToolButton(chip);
    remove->setObjectName("tag_remove");
    icons::Follow(remove, icons::Glyph::Close);
    remove->setIconSize(QSize(12, 12));
    remove->setToolTip(QString("Remove %1").arg(QString::fromStdString(tag)));
    connect(remove, &QToolButton::clicked, this, [this, tag] { Remove(tag); });
    chip_layout->addWidget(remove);
    flow->addWidget(chip);
    flow->Move(chip, static_cast<int>(chips_.size()));
    chips_.append(chip);
  }
  updateGeometry();
}

void TagEdit::ShowMenu(const std::string& tag, const QPoint& global) {
  QMenu menu(this);
  const QString name = QString::fromStdString(tag);
  menu.addAction(QString("Show games tagged %1").arg(name), this, [this, name] { emit TagClicked(name); });
  const bool is_folder = tag == FolderTag();
  if (IsFolderTag(tag) && !is_folder) {
    menu.addAction(icons::For(icons::Glyph::Folder), "Use as this game's folder", this, [this, tag] {
      PickFolder(tag);
      Rebuild();
      emit Changed();
    });
  } else if (is_folder && !pick_.empty()) {
    menu.addAction("Follow the folder tags' order", this, [this] {
      pick_.clear();
      Rebuild();
      emit Changed();
    });
  }
  menu.addSeparator();
  menu.addAction(QString("Remove %1").arg(name), this, [this, tag] { Remove(tag); });
  menu.exec(global);
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
  auto* chip = qobject_cast<QFrame*>(watched);
  if (chip != nullptr && chips_.contains(chip)) {
    const std::string tag = chip->property("tag").toString().toStdString();
    switch (event->type()) {
      case QEvent::ContextMenu:
        ShowMenu(tag, static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
      case QEvent::MouseButtonPress:
        if (static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) break;
        pressed_ = chip;
        return true;
      case QEvent::MouseButtonRelease:
        if (pressed_ != chip) break;
        pressed_ = nullptr;
        if (!chip->rect().contains(static_cast<QMouseEvent*>(event)->position().toPoint())) return true;
        // Only a folder pick: a click that filtered the library would read as the same kind of switch.
        if (IsFolderTag(tag) && tag != FolderTag()) {
          PickFolder(tag);
          Rebuild();
          emit Changed();
        }
        return true;
      default:
        break;
    }
    return QWidget::eventFilter(watched, event);
  }
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
