#include "SearchField.h"

#include <QAction>

#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui {

SearchField::SearchField(const QString& placeholder, QWidget* parent) : QLineEdit(parent) {
  setPlaceholderText(placeholder);
  icons::Follow(addAction(QIcon(), QLineEdit::LeadingPosition), icons::Glyph::Search,
                &theme::Tokens::text_muted);
  AddClearButton(this, "Clear the search");
}

void AddClearButton(QLineEdit* field, const QString& tip) {
  QAction* clear = field->addAction(QIcon(), QLineEdit::TrailingPosition);
  icons::Follow(clear, icons::Glyph::Close, &theme::Tokens::text_muted);
  clear->setToolTip(tip);
  clear->setVisible(!field->text().isEmpty());
  QObject::connect(clear, &QAction::triggered, field, &QLineEdit::clear);
  QObject::connect(field, &QLineEdit::textChanged, clear,
                   [clear](const QString& text) { clear->setVisible(!text.isEmpty()); });
}

}  // namespace mira_gui
