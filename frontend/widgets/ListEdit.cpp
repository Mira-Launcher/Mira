#include "ListEdit.h"

#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui {

QHBoxLayout* ListItemRow(QWidget* row, bool button_last) {
  row->setObjectName("list_row");
  row->setAttribute(Qt::WA_StyledBackground);
  auto* layout = new QHBoxLayout(row);
  // A tool button's glyph sits 9 px inside it, so text ends 9 px further in.
  layout->setContentsMargins(34, 2, button_last ? 9 : 18, 2);
  layout->setSpacing(8);
  return layout;
}

QToolButton* NewRemoveButton(QWidget* parent) {
  auto* remove = new QToolButton(parent);
  remove->setAutoRaise(true);
  icons::Follow(remove, icons::Glyph::Close, &theme::Tokens::text_muted);
  remove->setToolTip("Remove");
  return remove;
}

ListEdit::ListEdit(Kind kind, const QString& add_text, QWidget* parent)
    : QWidget(parent), kind_(kind) {
  setObjectName("list_edit");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  rows_ = new QVBoxLayout();
  rows_->setSpacing(0);
  layout->addLayout(rows_);

  more_ = new QPushButton(this);
  more_->setObjectName("list_more");
  more_->setCursor(Qt::PointingHandCursor);
  connect(more_, &QPushButton::clicked, this, [this] {
    show_all_ = true;
    Update();
  });
  layout->addWidget(more_);

  empty_ = new QLabel("None", this);
  theme::SetStyleProperty(empty_, "role", "subtle");

  add_ = new QPushButton(add_text, this);
  add_->setCursor(Qt::PointingHandCursor);
  connect(add_, &QPushButton::clicked, this, [this] {
    if (kind_ == Kind::Folder) {
      const QString folder =
          QFileDialog::getExistingDirectory(this, "Choose a folder", QDir::homePath());
      if (folder.isEmpty()) return;
      // Shown and stored the way the defaults are written, relative to home.
      const QString home = QDir::homePath();
      AddRow(folder.startsWith(home + "/") ? "~" + folder.mid(home.size()) : folder,
             /*edit=*/false);
      emit Changed();
      return;
    }
    AddRow({}, /*edit=*/true);
  });
  Update();
}

QStringList ListEdit::Items() const {
  QStringList items;
  for (int i = 0; i < rows_->count(); ++i) {
    if (const QWidget* row = rows_->itemAt(i)->widget()) {
      if (const auto* line = row->findChild<QLineEdit*>();
          line != nullptr && !line->text().trimmed().isEmpty()) {
        items << line->text().trimmed();
      }
    }
  }
  return items;
}

void ListEdit::SetItems(const QStringList& items) {
  while (QLayoutItem* item = rows_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  show_all_ = false;
  for (const QString& text : items) AddRow(text, /*edit=*/false);
  Update();
}

void ListEdit::SetReadOnly(bool read_only) {
  read_only_ = read_only;
  for (int i = 0; i < rows_->count(); ++i) {
    QWidget* row = rows_->itemAt(i)->widget();
    if (row == nullptr) continue;
    theme::SetStyleProperty(row, "inherited", read_only ? "true" : "false");
    if (auto* line = row->findChild<QLineEdit*>()) line->setReadOnly(read_only);
    if (auto* remove = row->findChild<QToolButton*>()) remove->setVisible(!read_only);
  }
}

void ListEdit::AddRow(const QString& text, bool edit) {
  auto* row = new QWidget(this);
  QHBoxLayout* layout = ListItemRow(row);
  theme::SetStyleProperty(row, "inherited", read_only_ ? "true" : "false");
  if (kind_ == Kind::Folder) {
    auto* icon = new QLabel(row);
    icons::Follow(icon, icons::Glyph::Folder, 16);
    layout->addWidget(icon);
  }
  // A frameless field, so an item can be corrected in place.
  auto* line = new QLineEdit(text, row);
  line->setObjectName("list_item");
  line->setFrame(false);
  line->setReadOnly(read_only_);
  connect(line, &QLineEdit::textEdited, this, &ListEdit::Changed);
  layout->addWidget(line, /*stretch=*/1);
  QToolButton* remove = NewRemoveButton(row);
  remove->setVisible(!read_only_);
  connect(remove, &QToolButton::clicked, this, [this, row] {
    rows_->removeWidget(row);
    row->deleteLater();
    Update();
    emit Changed();
  });
  layout->addWidget(remove);
  rows_->addWidget(row);
  if (edit) {
    show_all_ = true;  // a new item is never folded away
    line->setFocus();
  }
  Update();
}

void ListEdit::Update() {
  const int count = rows_->count();
  const bool folded = !show_all_ && count > kShortList;
  for (int i = 0; i < count; ++i) {
    if (QWidget* row = rows_->itemAt(i)->widget()) row->setVisible(!folded || i < kShownFolded);
  }
  more_->setText(QString("Show all %1").arg(count));
  more_->setVisible(folded);
  empty_->setVisible(count == 0);
  setVisible(count > 0);
}

}  // namespace mira_gui
