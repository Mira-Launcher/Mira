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

ListEdit::ListEdit(Kind kind, const QString& add_text, QWidget* parent) : QWidget(parent), kind_(kind) {
  setObjectName("list_edit");
  setAttribute(Qt::WA_StyledBackground);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(1, 1, 1, 1);
  layout->setSpacing(0);

  rows_ = new QVBoxLayout();
  rows_->setSpacing(0);
  layout->addLayout(rows_);

  empty_ = new QLabel("None", this);
  empty_->setProperty("role", "subtle");
  empty_->setContentsMargins(10, 6, 10, 6);
  layout->addWidget(empty_);

  auto* add = new QPushButton(icons::For(icons::Glyph::Plus, theme::Current().accent), add_text, this);
  add->setObjectName("list_add");
  add->setCursor(Qt::PointingHandCursor);
  connect(add, &QPushButton::clicked, this, [this] {
    if (kind_ == Kind::Folder) {
      const QString folder = QFileDialog::getExistingDirectory(this, "Choose a folder", QDir::homePath());
      if (folder.isEmpty()) return;
      // Shown and stored the way the defaults are written, relative to home.
      const QString home = QDir::homePath();
      AddRow(folder.startsWith(home + "/") ? "~" + folder.mid(home.size()) : folder, /*edit=*/false);
      emit Changed();
      return;
    }
    AddRow({}, /*edit=*/true);
  });
  layout->addWidget(add, 0, Qt::AlignLeft);
  UpdateEmpty();
}

QStringList ListEdit::Items() const {
  QStringList items;
  for (int i = 0; i < rows_->count(); ++i) {
    if (const QWidget* row = rows_->itemAt(i)->widget()) {
      if (const auto* line = row->findChild<QLineEdit*>(); line != nullptr && !line->text().trimmed().isEmpty()) {
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
  for (const QString& text : items) AddRow(text, /*edit=*/false);
  UpdateEmpty();
}

void ListEdit::AddRow(const QString& text, bool edit) {
  auto* row = new QWidget(this);
  row->setObjectName("list_row");
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(10, 0, 4, 0);
  layout->setSpacing(8);
  if (kind_ == Kind::Folder) {
    auto* icon = new QLabel(row);
    icon->setPixmap(icons::For(icons::Glyph::Folder, theme::Current().text_muted).pixmap(16, 16));
    layout->addWidget(icon);
  }
  // A frameless field, so an item can be corrected in place.
  auto* line = new QLineEdit(text, row);
  line->setObjectName("list_item");
  line->setFrame(false);
  connect(line, &QLineEdit::textEdited, this, &ListEdit::Changed);
  layout->addWidget(line, /*stretch=*/1);
  auto* remove = new QToolButton(row);
  remove->setAutoRaise(true);
  remove->setIcon(icons::For(icons::Glyph::Close, theme::Current().text_muted));
  remove->setToolTip("Remove");
  connect(remove, &QToolButton::clicked, this, [this, row] {
    rows_->removeWidget(row);
    row->deleteLater();
    UpdateEmpty();
    emit Changed();
  });
  layout->addWidget(remove);
  rows_->addWidget(row);
  UpdateEmpty();
  if (edit) line->setFocus();
}

void ListEdit::UpdateEmpty() { empty_->setVisible(rows_->count() == 0); }

}  // namespace mira_gui
