#include "KeyValueEdit.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "Icons.h"
#include "Theme.h"

namespace mira_gui {

KeyValueEdit::KeyValueEdit(const QString& add_text, QWidget* parent) : QWidget(parent) {
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
  connect(add, &QPushButton::clicked, this, [this] { AddRow({}, {}, /*edit=*/true); });
  layout->addWidget(add, 0, Qt::AlignLeft);
  UpdateEmpty();
}

std::map<std::string, std::string> KeyValueEdit::Values() const {
  std::map<std::string, std::string> values;
  for (int i = 0; i < rows_->count(); ++i) {
    const QWidget* row = rows_->itemAt(i)->widget();
    if (row == nullptr) continue;
    const QLineEdit* name = nullptr;
    const QLineEdit* value = nullptr;
    for (const QLineEdit* line : row->findChildren<QLineEdit*>()) {
      if (line->property("kv") == "kv_name") name = line;
      if (line->property("kv") == "kv_value") value = line;
    }
    if (name == nullptr || value == nullptr || name->text().trimmed().isEmpty()) continue;
    values[name->text().trimmed().toStdString()] = value->text().toStdString();
  }
  return values;
}

void KeyValueEdit::SetValues(const std::map<std::string, std::string>& values) {
  while (QLayoutItem* item = rows_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  for (const auto& [name, value] : values) {
    AddRow(QString::fromStdString(name), QString::fromStdString(value), /*edit=*/false);
  }
  UpdateEmpty();
}

void KeyValueEdit::AddRow(const QString& name, const QString& value, bool edit) {
  auto* row = new QWidget(this);
  row->setObjectName("list_row");
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(10, 0, 4, 0);
  layout->setSpacing(8);
  const auto field = [row](const QString& text, const char* object_name, const QString& placeholder,
                           const QString& accessible) {
    auto* line = new QLineEdit(text, row);
    line->setObjectName("list_item");  // ListEdit's style
    line->setProperty("kv", object_name);
    line->setFrame(false);
    line->setPlaceholderText(placeholder);
    line->setAccessibleName(accessible);
    return line;
  };
  auto* name_edit = field(name, "kv_name", "NAME", "Variable name");
  auto* value_edit = field(value, "kv_value", "value", "Value");
  for (QLineEdit* line : {name_edit, value_edit}) connect(line, &QLineEdit::textEdited, this, &KeyValueEdit::Changed);
  layout->addWidget(name_edit, /*stretch=*/2);
  auto* equals = new QLabel("=", row);
  equals->setProperty("role", "subtle");
  layout->addWidget(equals);
  layout->addWidget(value_edit, /*stretch=*/3);
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
  if (edit) name_edit->setFocus();
}

void KeyValueEdit::UpdateEmpty() { empty_->setVisible(rows_->count() == 0); }

}  // namespace mira_gui
